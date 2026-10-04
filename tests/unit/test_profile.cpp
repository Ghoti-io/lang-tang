// The sampling profiler (runtime-core's b/profile.h) on a real engine: what
// the profile of a Tang program says, on every tier.
//
// Most of these are exact. A small service asks for a sample at every poll,
// and counts, with its own frame walk written here and sharing nothing with
// the profiler, what the profile must say: the innermost frame's location once
// as `self`, and each distinct location of the walk once as `inclusive`. The
// profiler's report must be that count, location by location, with the JIT off
// and with every function tiering up at its first poll, and the two reports
// must be the same (a poll identity is the same on every tier, AD-18).
//
// The last group is statistical, with the timer: the hot loop of a program
// with a short prologue gets most of the samples. It extends the run until a
// floor of samples was taken and never asserts on a handful.

#include "exec_harness.h"
#include "test_helpers.h"

#include <ghoti.io/runtime-core/runtime-core.h>

#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

using tt::Compiled;
using tt::Config;
using tt::Context;
using tt::Mode;

namespace {

using Location = std::pair<std::string, int>;

struct Count {
  uint64_t self = 0;
  uint64_t inclusive = 0;
  bool operator==(const Count & o) const { return self == o.self && inclusive == o.inclusive; }
};

using Profile = std::map<Location, Count>;

Profile read_profile(GRCORE_Profiler * profiler, GRCORE_ProfileTotals * totals = nullptr) {
  std::vector<GRCORE_ProfileEntry> entries(1024);
  size_t n = 0;
  GRCORE_ProfileTotals t{};
  EXPECT_EQ(grcore_profiler_report(profiler, entries.data(), entries.size(), &n, &t), GRCORE_OK);
  EXPECT_LT(t.locations, entries.size());
  Profile p;
  for (size_t i = 0; i < n; i++) {
    Location l{entries[i].file != nullptr ? entries[i].file : "", entries[i].line};
    EXPECT_EQ(p.count(l), 0u) << "a location appears twice in a report";
    p[l] = Count{entries[i].self, entries[i].inclusive};
  }
  if (totals != nullptr) {
    *totals = t;
  }
  return p;
}

uint64_t total_self(const Profile & p) {
  uint64_t n = 0;
  for (const auto & e : p) {
    n += e.second.self;
  }
  return n;
}

/// Asks for a sample at every poll (an ACT handler posting the profiler's
/// kind), and counts what each sample must hold with a walk of its own (an
/// OBSERVE handler that looks at the same pending bit the profiler does).
struct EveryPoll {
  GRCORE_Profiler * profiler = nullptr;
  GRCORE_Port * port = nullptr;
  GRCORE_RequestKind kind = 0;
  GRCORE_RequestKind keep = 0; ///< Never cleared: every poll takes the slow path.
  Profile expected;
  uint64_t samples = 0;
  uint64_t no_frame = 0;

  EveryPoll() = default;
  EveryPoll(const EveryPoll &) = delete;
  EveryPoll & operator=(const EveryPoll &) = delete;
  ~EveryPoll() { grcore_port_release(port); }

  static const GRCORE_Key & poster() {
    static const GRCORE_Key k = {"profile test poster", GRCORE_CARDINALITY_ONE, GRCORE_PHASE_ACT,
        nullptr, &EveryPoll::post, nullptr, nullptr, nullptr};
    return k;
  }
  static const GRCORE_Key & oracle() {
    static const GRCORE_Key k = {"profile test oracle", GRCORE_CARDINALITY_ONE,
        GRCORE_PHASE_OBSERVE, nullptr, &EveryPoll::count, nullptr, nullptr, nullptr};
    return k;
  }

  /// The oracle is registered before the profiler, so that in an unshuffled
  /// poll it looks at the profiler's request before the profiler clears it.
  bool attach(GRCORE_Context * context, size_t capacity = 0) {
    if (grcore_context_port(context, &port) != GRCORE_OK ||
        grcore_context_request_kind(context, &poster(), &keep) != GRCORE_OK ||
        grcore_context_register(context, &poster(), this) != GRCORE_OK ||
        grcore_context_register(context, &oracle(), this) != GRCORE_OK ||
        grcore_profiler_attach(context, capacity, &profiler) != GRCORE_OK) {
      return false;
    }
    kind = grcore_profiler_kind(profiler);
    return grcore_port_post(port, keep) == GRCORE_OK && grcore_port_post(port, kind) == GRCORE_OK;
  }

 private:
  static void post(GRCORE_Context *, void * value, GRCORE_PollCall *) {
    auto * self = static_cast<EveryPoll *>(value);
    grcore_port_post(self->port, self->kind);
  }

  static void count(GRCORE_Context * context, void * value, GRCORE_PollCall * call) {
    auto * self = static_cast<EveryPoll *>(value);
    if (!grcore_pollcall_pending(call, self->kind)) {
      return;
    }
    self->samples++;
    GRCORE_FrameWalk walk;
    GRCORE_AbstractFrame frame;
    if (grcore_frame_walk_begin(context, &walk) != GRCORE_OK ||
        !grcore_frame_walk_next(&walk, &frame)) {
      self->no_frame++;
      return;
    }
    std::set<Location> seen;
    bool innermost = true;
    do {
      Location l{frame.location.file != nullptr ? frame.location.file : "", frame.location.line};
      if (innermost) {
        self->expected[l].self++;
        innermost = false;
      }
      if (seen.insert(l).second) {
        self->expected[l].inclusive++;
      }
    } while (grcore_frame_walk_next(&walk, &frame));
  }
};

std::string describe(const Profile & p) {
  std::string s;
  for (const auto & e : p) {
    s += e.first.first + ":" + std::to_string(e.first.second) + " self " +
        std::to_string(e.second.self) + " incl " + std::to_string(e.second.inclusive) + "\n";
  }
  return s;
}

struct Counted {
  Profile report;
  Profile expected;
  GRCORE_ProfileTotals totals{};
  uint64_t oracle_samples = 0;
  std::string output;
  GLTANG_JitStats stats{};
  bool ok = false;
};

/// Runs `source` with a sample at every poll under `threshold`, with optional
/// library templates ({name, source}), and returns the report beside the count.
Counted run_counted(const std::string & source, long threshold,
    const std::vector<std::pair<std::string, std::string>> & templates = {},
    Mode mode = Mode::Script) {
  Counted out;
  Compiled page(source, mode, "page.tang");
  EXPECT_TRUE(page.ok()) << page.error.message;
  if (!page.ok()) {
    return out;
  }
  Config config;
  config.jit_threshold = threshold;
  Context context(page.program, config);
  EXPECT_TRUE(context.ok());
  if (!context.ok()) {
    return out;
  }
  std::vector<std::unique_ptr<Compiled>> parts;
  for (const auto & t : templates) {
    parts.push_back(std::make_unique<Compiled>(t.second, Mode::Script, (t.first + ".tang").c_str()));
    EXPECT_TRUE(parts.back()->ok()) << parts.back()->error.message;
    EXPECT_EQ(gltang_library_add_template(context.library(), t.first.c_str(),
                  parts.back()->program, 1000000, GLTANG_SCOPE_EMPTY),
        GLTANG_OK);
  }
  EveryPoll every;
  EXPECT_TRUE(every.attach(context.context));
  EXPECT_TRUE(context.execute());
  out.output = context.raw();
  out.report = read_profile(every.profiler, &out.totals);
  out.expected = every.expected;
  out.oracle_samples = every.samples;
  EXPECT_EQ(every.no_frame, out.totals.no_frame);
  out.stats = context.jit_stats();
  out.ok = true;
  return out;
}

const char * const kCalls =
    "function leaf(n) {\n"            // 1
    "  i = 0;\n"                      // 2
    "  while (i < n) {\n"             // 3
    "    i = i + 1;\n"                // 4
    "  }\n"                           // 5
    "  return i;\n"                   // 6
    "}\n"                             // 7
    "function mid(n) {\n"             // 8
    "  v = leaf(n);\n"                // 9
    "  return v + 1;\n"               // 10
    "}\n"                             // 11
    "function rec(d) {\n"             // 12
    "  if (d == 0) {\n"               // 13
    "    return leaf(40);\n"          // 14
    "  }\n"                           // 15
    "  return rec(d - 1);\n"          // 16
    "}\n"                             // 17
    "print(mid(600));\n"              // 18
    "print(rec(5));\n";               // 19

}  // namespace

TEST(Profile, TheReportIsExactlyWhatAnIndependentWalkAtEverySampleSaysOnEveryTier) {
  Counted interpreted = run_counted(kCalls, 0);
  ASSERT_TRUE(interpreted.ok);
  Counted compiled = run_counted(kCalls, 1);
  ASSERT_TRUE(compiled.ok);
  for (const Counted * c : {&interpreted, &compiled}) {
    EXPECT_EQ(c->totals.samples, c->oracle_samples);
    EXPECT_GT(c->totals.samples, 400u) << "not a handful";
    EXPECT_EQ(c->totals.no_frame, 0u);
    EXPECT_EQ(c->totals.dropped, 0u);
    EXPECT_EQ(describe(c->report), describe(c->expected));
    EXPECT_EQ(total_self(c->report), c->totals.samples) << "every sample has one innermost frame";
  }
  // The same program has the same polls at the same places on every tier, so it
  // has the same profile.
  EXPECT_EQ(describe(interpreted.report), describe(compiled.report));
  EXPECT_EQ(interpreted.output, compiled.output);
#ifdef GLTANG_WITH_JIT
  EXPECT_GE(compiled.stats.functions_compiled, 1u) << "the JIT arm must not be vacuous";
  EXPECT_EQ(interpreted.stats.functions_compiled, 0u);
#endif
}

TEST(Profile, SelfIsTheInnermostFrameAndInclusiveCountsEveryCallerOnceEvenInARecursion) {
  Counted c = run_counted(kCalls, 0);
  ASSERT_TRUE(c.ok);
  const Location leaf_loop{"page.tang", 4};
  // Some line of leaf's loop is where most samples fall.
  uint64_t in_leaf = 0;
  for (int line = 2; line <= 6; line++) {
    auto it = c.report.find({"page.tang", line});
    if (it != c.report.end()) {
      in_leaf += it->second.self;
    }
  }
  EXPECT_GT(in_leaf, c.totals.samples / 2);
  (void)leaf_loop;
  // A caller is on the stack for every sample taken in its callees and is the
  // innermost frame for none of those.
  auto mid_call = c.report.find({"page.tang", 9});
  ASSERT_NE(mid_call, c.report.end());
  EXPECT_GT(mid_call->second.inclusive, mid_call->second.self);
  EXPECT_GT(mid_call->second.inclusive, 500u) << "mid(600) spends its time in leaf";
  // The recursion: rec calls itself five times, so at the bottom six frames are
  // at the recursive call's line. It still counts once per sample.
  auto recursive = c.report.find({"page.tang", 16});
  ASSERT_NE(recursive, c.report.end());
  EXPECT_LE(recursive->second.inclusive, c.totals.samples);
  EXPECT_EQ(describe(c.report), describe(c.expected));
  // Inclusive never exceeds the number of samples, and never falls below self.
  for (const auto & e : c.report) {
    EXPECT_LE(e.second.inclusive, c.totals.samples) << e.first.second;
    EXPECT_GE(e.second.inclusive, e.second.self) << e.first.second;
  }
}

TEST(Profile, TwoLoopsRunFourToOneAreSeenFourToOne) {
  const char * source =
      "function a(n) {\n"             // 1
      "  i = 0;\n"                    // 2
      "  while (i < n) {\n"           // 3
      "    i = i + 1;\n"              // 4
      "  }\n"                         // 5
      "  return i;\n"                 // 6
      "}\n"                           // 7
      "function b(n) {\n"             // 8
      "  i = 0;\n"                    // 9
      "  while (i < n) {\n"           // 10
      "    i = i + 1;\n"              // 11
      "  }\n"                         // 12
      "  return i;\n"                 // 13
      "}\n"                           // 14
      "print(a(4000));\n"             // 15
      "print(b(1000));\n";            // 16
  for (long threshold : {0L, 1L}) {
    Counted c = run_counted(source, threshold);
    ASSERT_TRUE(c.ok);
    uint64_t a_self = 0, b_self = 0;
    for (const auto & e : c.report) {
      if (e.first.second >= 1 && e.first.second <= 7) {
        a_self += e.second.self;
      }
      if (e.first.second >= 8 && e.first.second <= 14) {
        b_self += e.second.self;
      }
    }
    EXPECT_GT(a_self, b_self) << "threshold " << threshold;
    EXPECT_GT(b_self, 0u);
    // One back-edge poll per iteration: 4 to 1, give or take the entries.
    EXPECT_NEAR(static_cast<double>(a_self) / static_cast<double>(b_self), 4.0, 0.1)
        << "threshold " << threshold << "\n" << describe(c.report);
    EXPECT_GE(a_self + b_self, c.totals.samples * 9 / 10);
  }
}

TEST(Profile, ATemplateCallNestShowsTheCallersLineAndTheTemplatesFile) {
  const char * page =
      "function work() {\n"                   // 1
      "  use sub;\n"                          // 2
      "  return sub();\n"                     // 3
      "}\n"                                   // 4
      "print(work());\n";                     // 5
  const char * sub =
      "function spin(n) {\n"                  // 1
      "  k = 0;\n"                            // 2
      "  while (k < n) {\n"                   // 3
      "    k += 1;\n"                         // 4
      "  }\n"                                 // 5
      "  return k;\n"                         // 6
      "}\n"                                   // 7
      "spin(500);\n";                         // 8
  for (long threshold : {0L, 1L}) {
    Counted c = run_counted(page, threshold, {{"sub", sub}});
    ASSERT_TRUE(c.ok);
    EXPECT_EQ(describe(c.report), describe(c.expected));
    uint64_t in_template = 0;
    uint64_t at_call = 0;
    for (const auto & e : c.report) {
      if (e.first.first == "sub.tang") {
        in_template += e.second.self;
      }
      if (e.first.first == "page.tang" && e.first.second == 3) {
        at_call = e.second.inclusive;
      }
    }
    EXPECT_GT(in_template, 500u) << "the template's loop is where the samples are";
    EXPECT_GE(at_call, in_template) << "the page's call line is on the stack for all of them";
    EXPECT_EQ(describe(c.report), describe(c.expected));
  }
}

TEST(Profile, TheTableIsFixedAndALocationThatDoesNotFitIsCountedNotAllocated) {
  Compiled page(kCalls, Mode::Script, "page.tang");
  ASSERT_TRUE(page.ok());
  Context context(page.program);
  ASSERT_TRUE(context.ok());
  EveryPoll every;
  ASSERT_TRUE(every.attach(context.context, 3));
  uint64_t blocks = grcore_context_memory_blocks(context.context);
  ASSERT_TRUE(context.execute());
  GRCORE_ProfileTotals t{};
  Profile p = read_profile(every.profiler, &t);
  EXPECT_EQ(p.size(), 3u);
  EXPECT_EQ(t.locations, 3u);
  EXPECT_GT(t.dropped, 0u);
  EXPECT_EQ(t.samples, every.samples);
  // The run made objects (strings, frames) but the profiler made no block.
  EXPECT_GE(grcore_context_memory_blocks(context.context), blocks - 1);
}

TEST(Profile, AProfilerIsReadableWhileTheRunIsPausedAndTheRunCarriesOn) {
  Compiled page(kCalls, Mode::Script, "page.tang");
  ASSERT_TRUE(page.ok());
  Config config;
  config.fuel = 200;
  Context context(page.program, config);
  ASSERT_TRUE(context.ok());
  EveryPoll every;
  ASSERT_TRUE(every.attach(context.context));
  EXPECT_FALSE(context.execute());
  ASSERT_TRUE(context.paused());
  GRCORE_ProfileTotals t1{};
  Profile first = read_profile(every.profiler, &t1);
  EXPECT_GT(t1.samples, 0u);
  EXPECT_EQ(describe(first), describe(every.expected));
  EXPECT_TRUE(context.finished_after_raising(10000000));
  GRCORE_ProfileTotals t2{};
  Profile second = read_profile(every.profiler, &t2);
  EXPECT_GT(t2.samples, t1.samples);
  EXPECT_EQ(describe(second), describe(every.expected));
}

// ---------------------------------------------------------------------------
// The timer: a statistical claim, so it extends the run instead of trusting a
// handful of samples.
// ---------------------------------------------------------------------------

namespace {

const char * const kHot =
    "x = 1;\n"                         // 1: a short prologue
    "function spin(n) {\n"             // 2
    "  i = 0;\n"                       // 3
    "  t = 0;\n"                       // 4
    "  while (i < n) {\n"              // 5
    "    t = t + i;\n"                 // 6
    "    i = i + 1;\n"                 // 7
    "  }\n"                            // 8
    "  return t;\n"                    // 9
    "}\n"                              // 10
    "print(spin(%llu));\n";            // 11

struct Hot {
  double elapsed_ms = 0;
  uint64_t samples = 0;
  uint64_t in_loop = 0;
  uint64_t iterations = 0;
  Profile report;
};

/// Runs the hot program at `threshold` with a 1 ms timer, doubling the loop until
/// the floor of samples was taken (or the cap is reached).
Hot run_hot(long threshold, uint64_t floor_samples) {
  Hot hot;
  for (uint64_t n = 200000; n <= 400000000ull; n *= 4) {
    char source[512];
    std::snprintf(source, sizeof source, kHot, static_cast<unsigned long long>(n));
    Compiled page(source, Mode::Script, "hot.tang");
    EXPECT_TRUE(page.ok());
    Config config;
    config.jit_threshold = threshold;
    Context context(page.program, config);
    EXPECT_TRUE(context.ok());
    GRCORE_Profiler * profiler = nullptr;
    EXPECT_EQ(grcore_profiler_attach(context.context, 0, &profiler), GRCORE_OK);
    EXPECT_EQ(grcore_profiler_timer_start(profiler, 1000), GRCORE_OK);
    auto began = std::chrono::steady_clock::now();
    EXPECT_TRUE(context.execute());
    hot.elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
    EXPECT_EQ(grcore_profiler_timer_stop(profiler), GRCORE_OK);
    GRCORE_ProfileTotals totals{};
    hot.report = read_profile(profiler, &totals);
    hot.samples = totals.samples;
    hot.iterations = n;
    hot.in_loop = 0;
    for (const auto & e : hot.report) {
      if (e.first.first == "hot.tang" && e.first.second >= 5 && e.first.second <= 8) {
        hot.in_loop += e.second.self;
      }
    }
    if (totals.samples >= floor_samples) {
      break;
    }
  }
  return hot;
}

}  // namespace

TEST(ProfileTimer, TheHotLoopOfAProgramWithAShortPrologueGetsMostOfTheSamples) {
  for (long threshold : {0L, 1L}) {
    Hot hot = run_hot(threshold, 40);
    ASSERT_GE(hot.samples, 40u) << "threshold " << threshold << ": the run was extended to " << hot.iterations << " iterations and still took too few samples";
    // A sample consumes its request: the timer posts at most one a millisecond,
    // so there cannot be many more samples than milliseconds. (A profiler that
    // forgot to clear its request would sample at every poll after the first.)
    EXPECT_LE(static_cast<double>(hot.samples), hot.elapsed_ms + 20.0) << "threshold " << threshold;
    EXPECT_GT(hot.in_loop * 2, hot.samples) << "threshold " << threshold << "\n" << describe(hot.report);
  }
}

TEST(ProfileTimer, WithoutTheJitTheSameProgramHasTheSameShapeOfProfile) {
  Hot hot = run_hot(0, 40);
  ASSERT_GE(hot.samples, 40u);
  EXPECT_GT(hot.in_loop * 2, hot.samples);
  // The prologue is short: the top-level line is a sliver.
  uint64_t outside = hot.samples - hot.in_loop;
  EXPECT_LT(outside * 2, hot.samples);
}

TEST(ProfileTimer, DestroyingTheContextWhileTheTimerRunsIsClean) {
  for (int i = 0; i < 5; i++) {
    Compiled page("x = 0; while (x < 100) { x = x + 1; }", Mode::Script, "t.tang");
    ASSERT_TRUE(page.ok());
    Context context(page.program);
    ASSERT_TRUE(context.ok());
    GRCORE_Profiler * profiler = nullptr;
    ASSERT_EQ(grcore_profiler_attach(context.context, 0, &profiler), GRCORE_OK);
    ASSERT_EQ(grcore_profiler_timer_start(profiler, 100), GRCORE_OK);
    EXPECT_TRUE(context.execute());
    // The Context destructor destroys it with the timer still running.
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
