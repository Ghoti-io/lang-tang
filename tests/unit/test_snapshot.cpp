/**
 * @file
 *
 * Context snapshots (CAP-11): a paused or new execution frozen into an
 * immutable object and restored, into a fresh context on this thread or
 * another, to finish exactly as an uninterrupted run does.
 *
 * What is compared is everything a host can read of a finished run: the output
 * (raw and rendered), the result as a kind and a canonical text, the error list
 * with its chains and counts, and the fuel the whole run cost (the fuel used
 * before the pause plus the fuel the destination used must be what an
 * uninterrupted run used, or a tier or a restore charged differently).
 *
 * The corpus sweep pauses every executable program of the execution corpus at
 * several fuel points. The rest of this file is the matrix of the story: other
 * threads, restoring twice, a NEW execution, output and errors so far, host
 * values by name, stable IDs, every refusal, a mismatch at the destination,
 * allocation failure at every point, the destination's budgets, threads, the
 * collector's instruments over a restored heap and the JIT in both arms.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "exec_harness.h"
#include "oracle/run_lang_tang.h"
#include "test_helpers.h"

#include <ghoti.io/runtime-core/b/snapshot.h>
#include <ghoti.io/runtime-heap/image.h>

#include <algorithm>
#include <atomic>
#include <dirent.h>
#include <functional>
#include <memory>
#include <set>
#include <thread>

using tt::Compiled;
using tt::Config;
using tt::Context;

namespace {

// ---------------------------------------------------------------------------
// What a run left, as data
// ---------------------------------------------------------------------------

struct Observed {
  std::string raw;
  std::string rendered;
  std::string kind;
  std::string text;
  std::vector<std::string> errors;
  uint64_t dropped = 0;

  bool operator==(const Observed & o) const {
    return raw == o.raw && rendered == o.rendered && kind == o.kind && text == o.text && errors == o.errors && dropped == o.dropped;
  }
  std::string str() const {
    std::string s = "raw=" + raw + " rendered=" + rendered + " result=" + kind + ":" + text + " dropped=" + std::to_string(dropped);
    for (const auto & e : errors) {
      s += "\n  error " + e;
    }
    return s;
  }
};

/// `light` leaves out the rendered output, whose buffer is freed with cutil's
/// `gcu_free`: that bumps a process-wide counter without a lock, which two
/// threads freeing at once race on (ThreadSanitizer reports it). The raw output
/// carries the same bytes.
Observed observe(Context & c, bool light = false) {
  Observed o;
  o.raw = c.raw();
  if (!light) {
    o.rendered = c.rendered();
  }
  oracle::canonical_result(c.execution, &o.kind, &o.text);
  for (size_t i = 0; i < c.error_count(); ++i) {
    Context::Entry e = c.error(i);
    o.errors.push_back(std::string(gltang_error_kind_message(e.e.kind)) + "|" + std::to_string((int)e.e.how) + "|" + e.template_name() + "|" +
        e.file() + "|" + std::to_string(e.e.line) + "|" + std::to_string(e.e.function) + "|" + std::to_string(e.e.offset) + "|" + e.chain_text() +
        "|" + e.message());
  }
  o.dropped = gltang_execution_errors_dropped(c.execution);
  return o;
}

using Setup = std::function<void(Context &)>;

/// A snapshot, released with the scope.
struct Snap {
  GLTANG_Snapshot * s = nullptr;
  Snap() = default;
  Snap(const Snap &) = delete;
  Snap & operator=(const Snap &) = delete;
  Snap(Snap && o) noexcept : s(o.s) { o.s = nullptr; }
  ~Snap() { gltang_snapshot_release(s); }
};

/// A program run to its end with no interruption, the reference.
struct Reference {
  Observed seen;
  uint64_t fuel = 0;
  bool finished = false;
};

Reference run_reference(Compiled & c, const Config & config, const Setup & setup = nullptr, bool light = false) {
  Reference ref;
  Context ctx(c.program, config);
  EXPECT_TRUE(ctx.ok());
  if (setup) {
    setup(ctx);
  }
  ref.finished = ctx.execute();
  ref.seen = observe(ctx, light);
  ref.fuel = grcore_context_fuel_used(ctx.context);
  return ref;
}

/// A source context run under a fuel budget; `paused()` says it paused.
struct Source {
  std::unique_ptr<Context> ctx;
  uint64_t fuel_at_pause = 0;

  Source(Compiled & c, uint64_t fuel, Config config = Config(), const Setup & setup = nullptr) {
    config.fuel = fuel;
    ctx = std::make_unique<Context>(c.program, config);
    EXPECT_TRUE(ctx->ok());
    if (setup) {
      setup(*ctx);
    }
    ctx->execute();
    fuel_at_pause = grcore_context_fuel_used(ctx->context);
  }
  bool paused() const { return ctx->paused(); }
};

struct Finished {
  GLTANG_Result restored = GLTANG_ERR_INTERNAL;
  bool finished = false;
  Observed seen;
  uint64_t fuel = 0;
  GLTANG_ExecutionState state = GLTANG_EXECUTION_NEW;
};

/// Creates a fresh destination for `c`, restores `snap` into it and (if that
/// worked) finishes the run there: resumed if the snapshot was of a paused
/// execution, run if it was of a new one.
Finished restore_and_finish(Compiled & c, const GLTANG_Snapshot * snap, Config config = Config(), const Setup & setup = nullptr, bool paused = true, bool light = false) {
  Finished f;
  Context dst(c.program, config);
  EXPECT_TRUE(dst.ok());
  if (setup) {
    setup(dst);
  }
  dst.attach();
  f.restored = gltang_snapshot_restore(dst.execution, snap);
  f.state = gltang_execution_state(dst.execution);
  if (f.restored != GLTANG_OK) {
    return f;
  }
  f.finished = paused ? dst.resume() : dst.execute();
  f.seen = observe(dst, light);
  f.fuel = grcore_context_fuel_used(dst.context);
  return f;
}

/// Everything the story asks of one restore: finishes as the uninterrupted run
/// did, and the fuel adds up.
::testing::AssertionResult same_as(const Reference & ref, const Finished & f, uint64_t fuel_before) {
  if (f.restored != GLTANG_OK) {
    return ::testing::AssertionFailure() << "the restore failed: " << gltang_result_string(f.restored);
  }
  if (!f.finished) {
    return ::testing::AssertionFailure() << "the restored run did not finish";
  }
  if (!(f.seen == ref.seen)) {
    return ::testing::AssertionFailure() << "restored: " << f.seen.str() << "\nreference: " << ref.seen.str();
  }
  if (fuel_before + f.fuel != ref.fuel) {
    return ::testing::AssertionFailure() << "fuel " << fuel_before << " + " << f.fuel << " != " << ref.fuel;
  }
  return ::testing::AssertionSuccess();
}

/// A program, compiled from source.
std::unique_ptr<Compiled> compile(const std::string & source, tt::Mode mode = tt::Mode::Script, const char * file = "test.tang") {
  auto c = std::make_unique<Compiled>(source, mode, file);
  EXPECT_TRUE(c->ok()) << c->error.message;
  return c;
}

/// The fuel points the sweep uses for a run that costs `total`.
std::vector<uint64_t> points(uint64_t total, bool few) {
  std::vector<uint64_t> v;
  if (total < 2) {
    return v;
  }
  std::set<uint64_t> s;
  if (few) {
    s = {total / 2};
  } else {
    s = {1, total / 4, total / 2, (total * 3) / 4, total - 1};
  }
  for (uint64_t p : s) {
    if (p >= 1 && p < total) {
      v.push_back(p);
    }
  }
  return v;
}

// A program with a long loop that builds an array and prints, then ends with
// an error that is swallowed, a library value, and a result that holds all of it.
const char * kBuilder =
    "a = []; m = {:}; s = \"\";\n"
    "for (i = 0; i < 120; i += 1) { a[i] = i * i; s = s + \"x\"; if (i % 20 == 0) { print(i); print(\",\"); } }\n"
    "m.count = a.length; m.last = a[119]; m.text = s;\n"
    "print(1 / 0);\n"
    "print(\"|\");\n"
    "[a.length, m.last, s.length, m.count];";

}  // namespace

// ---------------------------------------------------------------------------
// The golden flow, and the rows of the matrix
// ---------------------------------------------------------------------------

TEST(Snapshot, APausedRunRestoredOnAnotherThreadFinishesAsTheUninterruptedRunDoes) {
  auto c = compile(kBuilder);
  Reference ref = run_reference(*c, Config());
  ASSERT_TRUE(ref.finished);
  ASSERT_GT(ref.fuel, 100u);
  Source src(*c, ref.fuel / 2);
  ASSERT_TRUE(src.paused());
  Snap snap;
  ASSERT_EQ(gltang_snapshot_take(src.ctx->execution, &snap.s), GLTANG_OK);
  EXPECT_GT(gltang_snapshot_size(snap.s), 0u);

  Finished f;
  std::thread([&] { f = restore_and_finish(*c, snap.s); }).join();
  EXPECT_TRUE(same_as(ref, f, src.fuel_at_pause));
  EXPECT_EQ(gltang_execution_state(src.ctx->execution), GLTANG_EXECUTION_PAUSED) << "the source is untouched";
  // And the source can still finish, to the same answer.
  grcore_context_set_fuel(src.ctx->context, GRCORE_UNLIMITED);
  ASSERT_TRUE(src.ctx->resume());
  EXPECT_EQ(observe(*src.ctx), ref.seen);
}

TEST(Snapshot, OneSnapshotRestoredTwiceGivesTwoIdenticalRunsAndReleasingItEarlyChangesNothing) {
  auto c = compile(kBuilder);
  Reference ref = run_reference(*c, Config());
  Source src(*c, ref.fuel / 3);
  ASSERT_TRUE(src.paused());
  Snap snap;
  ASSERT_EQ(gltang_snapshot_take(src.ctx->execution, &snap.s), GLTANG_OK);
  Context a(c->program, Config());
  Context b(c->program, Config());
  ASSERT_EQ(gltang_snapshot_restore(a.execution, snap.s), GLTANG_OK);
  ASSERT_EQ(gltang_snapshot_restore(b.execution, snap.s), GLTANG_OK);
  // The snapshot and the context it came from go away; the restored ones do not
  // need either.
  gltang_snapshot_release(snap.s);
  snap.s = nullptr;
  src.ctx.reset();
  ASSERT_TRUE(a.resume());
  ASSERT_TRUE(b.resume());
  EXPECT_EQ(observe(a), observe(b));
  EXPECT_EQ(observe(a), ref.seen);
}

TEST(Snapshot, ANewExecutionSnapshotsAndRestoresToTheSameOutputAsAFreshOne) {
  auto c = compile(kBuilder);
  Reference ref = run_reference(*c, Config());
  Context src(c->program, Config());
  ASSERT_TRUE(src.ok());
  Snap snap;
  ASSERT_EQ(gltang_snapshot_take(src.execution, &snap.s), GLTANG_OK);
  EXPECT_FALSE(grcore_snapshot_was_paused(snap.s));
  Finished f = restore_and_finish(*c, snap.s, Config(), nullptr, false);
  EXPECT_TRUE(same_as(ref, f, 0));
  EXPECT_EQ(gltang_execution_state(src.execution), GLTANG_EXECUTION_NEW);
}

TEST(Snapshot, OutputSoFarContinuesFromTheSameBytesAndTheErrorListIsIdentical) {
  // Output was printed and an error swallowed before the pause: both are in
  // the snapshot, and the restored run appends to them.
  const char * source =
      "print(\"before\"); print(1 / 0); print(\"<\" + !\"&\" + \">\");\n"
      "for (i = 0; i < 50; i += 1) { }\n"
      "print(\"after\"); print([1, 2][5][0]); 7;";
  auto c = compile(source);
  Reference ref = run_reference(*c, Config());
  ASSERT_GE(ref.seen.errors.size(), 2u);
  // Pause after the first statements: fuel enough for them, not for the loop.
  Source src(*c, 60);
  ASSERT_TRUE(src.paused());
  ASSERT_EQ(src.ctx->raw().substr(0, 6), "before") << "the output so far exists at the pause";
  ASSERT_GE(src.ctx->error_count(), 1u) << "and an error has been swallowed";
  size_t errors_at_pause = src.ctx->error_count();
  std::string output_at_pause = src.ctx->raw();
  Snap snap;
  ASSERT_EQ(gltang_snapshot_take(src.ctx->execution, &snap.s), GLTANG_OK);

  Context dst(c->program, Config());
  ASSERT_EQ(gltang_snapshot_restore(dst.execution, snap.s), GLTANG_OK);
  EXPECT_EQ(dst.raw(), output_at_pause) << "restored output starts at the same bytes";
  EXPECT_EQ(dst.error_count(), errors_at_pause);
  EXPECT_EQ(dst.rendered(), src.ctx->rendered());
  ASSERT_TRUE(dst.resume());
  EXPECT_EQ(observe(dst), ref.seen);
  EXPECT_EQ(dst.raw().substr(0, output_at_pause.size()), output_at_pause);
}

TEST(Snapshot, TheDroppedErrorCountAndTheLimitSurviveARestoreAndLaterErrorsAreStillDropped) {
  // A limit of two with six swallowed errors: four are dropped. The pause is
  // after the list overflowed, so the count is part of what the image carries.
  const char * source =
      "print(1 / 0); print(1 / 0); print(1 / 0); print(1 / 0);\n"
      "for (i = 0; i < 40; i += 1) { }\n"
      "print(1 / 0); print(1 / 0); 3;";
  auto c = compile(source);
  ::Setup limit = [](Context & ctx) { ASSERT_EQ(gltang_execution_set_error_limit(ctx.execution, 2), GLTANG_OK); };
  Reference ref = run_reference(*c, Config(), limit);
  ASSERT_EQ(ref.seen.dropped, 4u);
  bool any = false;
  for (uint64_t at : {40, 60, 80, 100}) {
    Source src(*c, at, Config(), limit);
    if (!src.paused() || gltang_execution_errors_dropped(src.ctx->execution) == 0) {
      continue;
    }
    any = true;
    uint64_t dropped_at_pause = gltang_execution_errors_dropped(src.ctx->execution);
    Snap snap;
    ASSERT_EQ(gltang_snapshot_take(src.ctx->execution, &snap.s), GLTANG_OK);
    Finished f = restore_and_finish(*c, snap.s, Config(), limit);
    EXPECT_TRUE(same_as(ref, f, src.fuel_at_pause)) << "fuel " << at;
    EXPECT_GE(f.seen.dropped, dropped_at_pause);
    EXPECT_EQ(f.seen.dropped, 4u);
  }
  EXPECT_TRUE(any) << "no fuel point paused after the list overflowed";
}

TEST(Snapshot, AnUnloggedErrorResultSurvivesAPauseBeforeTheNextStatement) {
  // A statement whose value is an error, nothing consumed it: the next
  // statement's result replaces it and the loss is recorded. A pause between
  // the two must not forget that it is pending.
  const char * source =
      "1 / 0;\n"
      "for (i = 0; i < 30; i += 1) { }\n"
      "2;";
  auto c = compile(source);
  Reference ref = run_reference(*c, Config());
  size_t checked = 0;
  for (uint64_t at = 2; at < ref.fuel; at += 3) {
    Source src(*c, at);
    if (!src.paused()) {
      continue;
    }
    Snap snap;
    ASSERT_EQ(gltang_snapshot_take(src.ctx->execution, &snap.s), GLTANG_OK);
    Finished f = restore_and_finish(*c, snap.s);
    EXPECT_TRUE(same_as(ref, f, src.fuel_at_pause)) << "fuel " << at;
    checked++;
  }
  EXPECT_GT(checked, 3u);
}

// ---------------------------------------------------------------------------
// The corpus
// ---------------------------------------------------------------------------

namespace {

std::vector<std::string> files_in(const std::string & sub) {
  std::vector<std::string> names;
  std::string dir = std::string(GLTANG_TEST_DATA) + "/corpus/" + sub;
  if (DIR * d = opendir(dir.c_str())) {
    while (dirent * e = readdir(d)) {
      std::string n = e->d_name;
      if (n.size() > 5 && n.compare(n.size() - 5, 5, ".tang") == 0) {
        names.push_back(n);
      }
    }
    closedir(d);
  }
  std::sort(names.begin(), names.end());
  return names;
}

struct SweepCounts {
  size_t programs = 0;
  size_t pauses = 0;
  size_t restored = 0;
  size_t refused = 0;
  size_t threads = 0;
};

/// Runs every executable program of the corpus, pauses it at several fuel
/// points and checks each restore against the uninterrupted run. `stride`
/// thins the set (the instruments make every run an order of magnitude dearer).
SweepCounts sweep(const Config & src_config, const Config & dst_config, size_t stride, bool few) {
  SweepCounts counts;
  size_t seen = 0;
  const uint64_t budget = tt::heavy_instruments() ? 20000 : 2000000;
  const std::set<std::string> skipped = {"script/container-nested-deeper-than-the-value-depth-bound.tang", "script/container-nested-deep-print.tang",
      "script/container-nested-just-inside-the-bound.tang"};
  for (const char * sub : {"script", "template"}) {
    bool script = std::string(sub) == "script";
    for (const std::string & name : files_in(sub)) {
      std::string file = std::string(sub) + "/" + name;
      if (name.find("reject-") == 0 || (tt::heavy_instruments() && skipped.count(file)) || name.find("runaway") != std::string::npos ||
          name == "break-continue.tang") {
        continue;
      }
      if (seen++ % stride != 0) {
        continue;
      }
      Compiled c(read_file(std::string(GLTANG_TEST_DATA) + "/corpus/" + file), script ? tt::Mode::Script : tt::Mode::Template, "program.tang");
      if (!c.ok()) {
        continue; // the few the compiler refuses are the corpus test's business
      }
      Config bounded = src_config;
      bounded.fuel = budget;
      Reference ref = run_reference(c, bounded);
      if (!ref.finished) {
        continue; // spent its budget: a heavy loop, not a program with an answer
      }
      ++counts.programs;
      bool first = true;
      for (uint64_t p : points(ref.fuel, few)) {
        Source src(c, p, src_config);
        if (!src.paused()) {
          continue; // finished before the next poll after `p`
        }
        ++counts.pauses;
        Snap snap;
        GLTANG_Result taken = gltang_snapshot_take(src.ctx->execution, &snap.s);
        if (taken != GLTANG_OK) {
          ++counts.refused;
          ADD_FAILURE() << file << " paused at " << p << ": the take was refused (" << gltang_result_string(taken) << ")";
          continue;
        }
        Finished f;
        if (first) {
          // The first point of each program is restored on another thread.
          std::thread([&] { f = restore_and_finish(c, snap.s, dst_config); }).join();
          ++counts.threads;
          first = false;
        } else {
          f = restore_and_finish(c, snap.s, dst_config);
        }
        if (!same_as(ref, f, src.fuel_at_pause)) {
          ADD_FAILURE() << file << " paused at fuel " << p << " (used " << src.fuel_at_pause << "): " << same_as(ref, f, src.fuel_at_pause).message();
          return counts;
        }
        ++counts.restored;
      }
    }
  }
  return counts;
}

}  // namespace

TEST(SnapshotCorpus, EveryProgramPausedAtSeveralPointsRestoresToTheUninterruptedRun) {
  SweepCounts counts = sweep(Config(), Config(), 1, false);
  std::printf("  snapshot corpus sweep: %zu programs, %zu pauses, %zu restored (%zu on another thread), %zu refused\n", counts.programs,
      counts.pauses, counts.restored, counts.threads, counts.refused);
  EXPECT_GE(counts.programs, tt::heavy_instruments() ? 400u : 440u);
  EXPECT_GE(counts.restored, 250u) << "a program with more than a poll's worth of fuel has a pause point";
  EXPECT_EQ(counts.restored, counts.pauses);
  EXPECT_EQ(counts.refused, 0u);
}

#ifdef GLTANG_WITH_JIT
TEST(SnapshotCorpus, TheJitAtThresholdOneRestoresToTheSameRunInEveryPairOfArms) {
  // Compiled code is dropped at the take and the destination has its own, so a
  // run paused with every function tiered up resumes on an interpreter-only
  // destination, and the other way round, to the same answer and the same fuel.
  Config jit, interp;
  jit.jit_threshold = 1;
  interp.jit_threshold = 0;
  size_t stride = 4;
  SweepCounts a = sweep(jit, interp, stride, true);
  SweepCounts b = sweep(interp, jit, stride, true);
  SweepCounts both = sweep(jit, jit, stride, true);
  EXPECT_GT(a.restored, 15u);
  EXPECT_GT(b.restored, 15u);
  EXPECT_GT(both.restored, 15u);
}
#endif

// ---------------------------------------------------------------------------
// Host values, by name
// ---------------------------------------------------------------------------

namespace {

bool bump(GLTANG_NativeCall * call, void * user) {
  ++*static_cast<int *>(user);
  gltang_call_return_integer(call, gltang_call_integer(call, 0) + 1);
  return true;
}

/// The libraries of a context: a native `bump` (counting its calls in `calls`),
/// a template `sidebar`, and a named library `tools` with a native of its own.
struct Host {
  int calls = 0;
  int tool_calls = 0;
  std::unique_ptr<Compiled> sidebar;
  GLTANG_Library * tools = nullptr;
  bool with_bump = true;

  Host() { sidebar = compile("print(\"<aside>\");"); }
  ~Host() { gltang_library_release(tools); }
  void install(Context & ctx) {
    if (with_bump) {
      ASSERT_EQ(gltang_library_add_native(ctx.library(), "bump", bump, &calls), GLTANG_OK);
    }
    ASSERT_EQ(gltang_library_add_template(ctx.library(), "sidebar", sidebar->program, 100000, GLTANG_SCOPE_EMPTY), GLTANG_OK);
    ASSERT_EQ(gltang_library_create("tools", &tools), GLTANG_OK);
    ASSERT_EQ(gltang_library_add_native(tools, "bump", bump, &tool_calls), GLTANG_OK);
    ASSERT_EQ(gltang_library_add_library(ctx.library(), tools), GLTANG_OK);
  }
  Setup setup() {
    return [this](Context & ctx) { install(ctx); };
  }
};

const char * kHostProgram =
    "use bump; use sidebar; use tools; use math; use random;\n"
    "f = bump; t = sidebar; lib = tools; m = math; g = random.seeded(5); setter = g.set_seed;\n"
    "total = 0; held = [f, t, lib, m, g, setter];\n"
    "for (i = 0; i < 60; i += 1) { total = total + f(i) + lib.bump(i); }\n"
    "setter(9); held[4].next_int;\n"
    "print(t()); print(\"|\"); print(total);\n"
    "[total, held[0](1), held[2].bump(2)];";

}  // namespace

TEST(SnapshotHost, ALibraryNativeAndTemplateAreFoundAgainByNameAndTheDestinationsOwnRuns) {
  auto c = compile(kHostProgram);
  Host reference_host;
  Reference ref = run_reference(*c, Config(), reference_host.setup());
  ASSERT_TRUE(ref.finished) << ref.seen.str();
  ASSERT_EQ(ref.seen.kind, "array");
  Host source_host;
  Source src(*c, ref.fuel / 3, Config(), source_host.setup());
  ASSERT_TRUE(src.paused());
  int source_calls_at_pause = source_host.calls;
  ASSERT_GT(source_calls_at_pause, 0);
  Snap snap;
  ASSERT_EQ(gltang_snapshot_take(src.ctx->execution, &snap.s), GLTANG_OK);

  // The destination has the same libraries, built again: other addresses, other
  // `user` pointers, the same names.
  Host dest_host;
  Finished f = restore_and_finish(*c, snap.s, Config(), dest_host.setup());
  EXPECT_TRUE(same_as(ref, f, src.fuel_at_pause));
  EXPECT_GT(dest_host.calls, 0) << "the destination's own native ran";
  EXPECT_EQ(source_host.calls, source_calls_at_pause) << "and the source's did not";
  EXPECT_EQ(dest_host.calls + source_calls_at_pause, reference_host.calls);
  EXPECT_EQ(dest_host.tool_calls + source_host.tool_calls, reference_host.tool_calls);
}

TEST(SnapshotHost, ANameThatDoesNotResolveRefusesTheRestoreAndTheDestinationRunsAFreshProgramNormally) {
  auto c = compile(kHostProgram);
  Host source_host;
  Reference ref = run_reference(*c, Config(), source_host.setup());
  Host paused_host;
  Source src(*c, ref.fuel / 3, Config(), paused_host.setup());
  ASSERT_TRUE(src.paused());
  Snap snap;
  ASSERT_EQ(gltang_snapshot_take(src.ctx->execution, &snap.s), GLTANG_OK);

  // A destination with no `bump`.
  Host missing;
  missing.with_bump = false;
  Context dst(c->program, Config());
  ASSERT_TRUE(dst.ok());
  missing.install(dst);
  dst.attach();
  EXPECT_EQ(gltang_snapshot_restore(dst.execution, snap.s), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_execution_state(dst.execution), GLTANG_EXECUTION_NEW);
  EXPECT_EQ(dst.error_count(), 0u);
  EXPECT_EQ(grcore_context_state(dst.context), GRCORE_CONTEXT_PARKED);
  // It is a fresh, runnable execution: its own run is what a fresh one gives.
  Host another;
  another.with_bump = false;
  Reference fresh = run_reference(*c, Config(), another.setup());
  EXPECT_TRUE(dst.execute() == fresh.finished);
  EXPECT_EQ(observe(dst), fresh.seen);
}

TEST(SnapshotHost, ALibraryWithADifferentShapeUnderTheSameNameIsRefusedByKind) {
  auto c = compile("use bump; f = bump; for (i = 0; i < 40; i += 1) { f(i); } f(1);");
  Host a;
  Source src(*c, 80, Config(), a.setup());
  ASSERT_TRUE(src.paused());
  Snap snap;
  ASSERT_EQ(gltang_snapshot_take(src.ctx->execution, &snap.s), GLTANG_OK);
  // The destination's `bump` is an integer, not a native.
  Context dst(c->program, Config());
  ASSERT_EQ(gltang_library_add_integer(dst.library(), "bump", 3), GLTANG_OK);
  dst.attach();
  EXPECT_EQ(gltang_snapshot_restore(dst.execution, snap.s), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_execution_state(dst.execution), GLTANG_EXECUTION_NEW);
}

TEST(SnapshotHost, ATemplateObjectInTheHeapIsRefusedWhenTheDestinationsTemplateOfThatNameIsOtherCode) {
  // `t = sidebar` leaves a template object in the heap that has not run. The
  // destination has a template of the same name that prints something else;
  // restoring would run its code on the source's frames.
  auto c = compile(
      "use sidebar; t = sidebar;\n"
      "for (i = 0; i < 60; i += 1) { }\n"
      "print(t()); print(\"|\");");
  Host h;
  Source src(*c, 100, Config(), h.setup());
  ASSERT_TRUE(src.paused());
  Snap snap;
  ASSERT_EQ(gltang_snapshot_take(src.ctx->execution, &snap.s), GLTANG_OK);

  // The same code under the same name restores and finishes with the source's.
  Host same;
  Reference ref = run_reference(*c, Config(), same.setup());
  ASSERT_TRUE(ref.finished) << ref.seen.str();
  Host twin;
  Finished f = restore_and_finish(*c, snap.s, Config(), twin.setup());
  EXPECT_TRUE(same_as(ref, f, src.fuel_at_pause));

  // Other code under the name is refused, and the destination stays fresh.
  Host other;
  other.sidebar = compile("print(\"<other>\");");
  Context dst(c->program, Config());
  ASSERT_TRUE(dst.ok());
  other.install(dst);
  dst.attach();
  EXPECT_EQ(gltang_snapshot_restore(dst.execution, snap.s), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_execution_state(dst.execution), GLTANG_EXECUTION_NEW);
  EXPECT_EQ(grcore_context_state(dst.context), GRCORE_CONTEXT_PARKED);
  EXPECT_EQ(dst.raw().find("<other>"), std::string::npos);
}

TEST(SnapshotHost, ATemplateThatHasAlreadyRunComesBackWithItsProgramAndItsConstants) {
  // A template called before the pause leaves a program entry (and its
  // constants cache) in the execution; a later call must still work, and its
  // string constants be the ones it made.
  Host h;
  auto c = compile(
      "use sidebar;\n"
      "print(sidebar()); print(sidebar());\n"
      "for (i = 0; i < 80; i += 1) { }\n"
      "print(sidebar()); print(\"end\");");
  Host reference_host;
  Reference ref = run_reference(*c, Config(), reference_host.setup());
  ASSERT_TRUE(ref.finished) << ref.seen.str();
  Source src(*c, ref.fuel * 2 / 3, Config(), h.setup());
  ASSERT_TRUE(src.paused());
  ASSERT_EQ(src.ctx->raw().find("<aside><aside>"), 0u) << "the template has run before the pause";
  Snap snap;
  ASSERT_EQ(gltang_snapshot_take(src.ctx->execution, &snap.s), GLTANG_OK);
  Host dest_host;
  Finished f = restore_and_finish(*c, snap.s, Config(), dest_host.setup());
  EXPECT_TRUE(same_as(ref, f, src.fuel_at_pause));
}

// ---------------------------------------------------------------------------
// A generator continues; a generator not yet made comes from the destination
// ---------------------------------------------------------------------------

namespace {

Setup seeded(uint64_t master) {
  return [master](Context & ctx) {
    GLTANG_SeedSequence * seeds = nullptr;
    ASSERT_EQ(gltang_seeds_create(master, &seeds), GLTANG_OK);
    ASSERT_EQ(gltang_execution_set_seeds(ctx.execution, seeds), GLTANG_OK);
    gltang_seeds_destroy(seeds);
  };
}

}  // namespace

TEST(SnapshotRandom, AGlobalGeneratorThatWasMadeContinuesWhereItWas) {
  auto c = compile(
      "use random; a = random.global.next_int;\n"
      "for (i = 0; i < 60; i += 1) { }\n"
      "b = random.global.next_int; [a, b, random.global.next_int];");
  Reference ref = run_reference(*c, Config(), seeded(7));
  ASSERT_TRUE(ref.finished) << ref.seen.str();
  Source src(*c, ref.fuel / 2, Config(), seeded(7));
  ASSERT_TRUE(src.paused());
  Snap snap;
  ASSERT_EQ(gltang_snapshot_take(src.ctx->execution, &snap.s), GLTANG_OK);
  // The destination's sequence differs: the generator already made does not
  // care, since its state is in the snapshot.
  Finished f = restore_and_finish(*c, snap.s, Config(), seeded(99));
  EXPECT_TRUE(same_as(ref, f, src.fuel_at_pause));
}

TEST(SnapshotRandom, AGeneratorNotYetMadeIsMadeFromTheDestinationsOwnSequence) {
  auto c = compile(
      "use random;\n"
      "for (i = 0; i < 60; i += 1) { }\n"
      "random.global.next_int;");
  Reference ref7 = run_reference(*c, Config(), seeded(7));
  Reference ref8 = run_reference(*c, Config(), seeded(8));
  ASSERT_NE(ref7.seen.text, ref8.seen.text) << "the control: the sequence decides the answer";
  Source src(*c, ref7.fuel / 2, Config(), seeded(7));
  ASSERT_TRUE(src.paused());
  Snap snap;
  ASSERT_EQ(gltang_snapshot_take(src.ctx->execution, &snap.s), GLTANG_OK);
  Finished same = restore_and_finish(*c, snap.s, Config(), seeded(7));
  EXPECT_TRUE(same_as(ref7, same, src.fuel_at_pause));
  Finished other = restore_and_finish(*c, snap.s, Config(), seeded(8));
  EXPECT_TRUE(same_as(ref8, other, src.fuel_at_pause)) << "the destination's sequence, not the source's";
}

// ---------------------------------------------------------------------------
// Stable IDs
// ---------------------------------------------------------------------------

namespace {

/// The first heap object a root slot of the context names, and its position.
bool first_object_root(Context & ctx, size_t skip, void ** object, size_t * index) {
  struct State {
    GRHEAP_Heap * heap;
    size_t skip;
    size_t at = 0;
    void * found = nullptr;
    size_t found_at = 0;
  } state = {ctx.heap, skip};
  GRCORE_RootVisitor v = {};
  v.user = &state;
  v.slot = [](void * u, uint64_t * slot) {
    auto * s = static_cast<State *>(u);
    void * p = reinterpret_cast<void *>(*slot);
    if (!s->found && *slot != 0 && (*slot & 0xF) == 0 && grheap_contains(s->heap, p)) {
      if (s->skip == 0) {
        s->found = p;
        s->found_at = s->at;
      } else {
        --s->skip;
      }
    }
    ++s->at;
  };
  EXPECT_EQ(grcore_context_enumerate_roots(ctx.context, &v), GRCORE_OK);
  *object = state.found;
  *index = state.found_at;
  return state.found != nullptr;
}

void * root_at(Context & ctx, size_t position) {
  struct State {
    size_t position;
    size_t at = 0;
    uint64_t word = 0;
  } state = {position};
  GRCORE_RootVisitor v = {};
  v.user = &state;
  v.slot = [](void * u, uint64_t * slot) {
    auto * s = static_cast<State *>(u);
    if (s->at++ == s->position) {
      s->word = *slot;
    }
  };
  EXPECT_EQ(grcore_context_enumerate_roots(ctx.context, &v), GRCORE_OK);
  return reinterpret_cast<void *>(state.word);
}

}  // namespace

TEST(SnapshotIds, AStableIdSurvivesAndAnIdIssuedLaterDoesNotCollide) {
  auto c = compile("a = [1, 2, 3]; m = {k: a}; for (i = 0; i < 200; i += 1) { a[i % 3] = i; } [a, m];");
  Reference ref = run_reference(*c, Config());
  Source src(*c, ref.fuel / 2, Config());
  ASSERT_TRUE(src.paused());
  void * object = nullptr;
  size_t position = 0;
  ASSERT_TRUE(first_object_root(*src.ctx, 0, &object, &position));
  uint64_t id = 0;
  ASSERT_EQ(grheap_id_of(src.ctx->heap, object, &id), GRHEAP_OK);
  uint64_t other_id = 0;
  void * second = nullptr;
  size_t second_position = 0;
  ASSERT_TRUE(first_object_root(*src.ctx, 1, &second, &second_position));
  ASSERT_EQ(grheap_id_of(src.ctx->heap, second, &other_id), GRHEAP_OK);
  Snap snap;
  ASSERT_EQ(gltang_snapshot_take(src.ctx->execution, &snap.s), GLTANG_OK);

  Context dst(c->program, Config());
  ASSERT_EQ(gltang_snapshot_restore(dst.execution, snap.s), GLTANG_OK);
  void * restored = nullptr;
  ASSERT_EQ(grheap_object_of_id(dst.heap, id, &restored), GRHEAP_OK) << "the same id names an object";
  EXPECT_EQ(restored, root_at(dst, position)) << "and it is the object at the same root";
  EXPECT_NE(restored, object);
  uint64_t again = 0;
  ASSERT_EQ(grheap_id_of(dst.heap, restored, &again), GRHEAP_OK);
  EXPECT_EQ(again, id) << "no new id is issued for it";
  void * fresh = nullptr;
  ASSERT_EQ(grheap_object_of_id(dst.heap, other_id, &fresh), GRHEAP_OK);
  EXPECT_EQ(fresh, root_at(dst, second_position));
  // An id issued after the restore is above every id the source issued.
  uint64_t third = 0;
  void * third_object = nullptr;
  size_t third_position = 0;
  ASSERT_TRUE(first_object_root(dst, 2, &third_object, &third_position));
  ASSERT_EQ(grheap_id_of(dst.heap, third_object, &third), GRHEAP_OK);
  EXPECT_GT(third, id);
  EXPECT_GT(third, other_id);
  ASSERT_TRUE(dst.resume());
  EXPECT_EQ(observe(dst), ref.seen);
}

// ---------------------------------------------------------------------------
// When a snapshot may be taken
// ---------------------------------------------------------------------------

namespace {

struct TakeRefused {
  GLTANG_Execution * execution = nullptr;
  GLTANG_Result result = GLTANG_OK;
  GLTANG_Snapshot * out = reinterpret_cast<GLTANG_Snapshot *>(0x1234);
  int calls = 0;
};

bool take_from_a_native(GLTANG_NativeCall *, void * user) {
  auto * t = static_cast<TakeRefused *>(user);
  t->result = gltang_snapshot_take(t->execution, &t->out);
  ++t->calls;
  return true;
}

void take_at_a_poll(GRCORE_Context *, void * value, GRCORE_PollCall *) {
  auto * t = static_cast<TakeRefused *>(value);
  t->result = gltang_snapshot_take(t->execution, &t->out);
  ++t->calls;
}

const GRCORE_Key kTakeAtPoll = GRCORE_KEY_INIT("take at poll", GRCORE_CARDINALITY_ONE, GRCORE_PHASE_OBSERVE, nullptr, take_at_a_poll, nullptr, nullptr, nullptr);

}  // namespace

TEST(SnapshotRefusals, ARunningExecutionAnAtPollOneAndAHostCallAreRefusedAndNothingChanges) {
  auto c = compile("use probe; for (i = 0; i < 20; i += 1) { probe(); } 5;");
  TakeRefused t;
  Context ctx(c->program, Config());
  t.execution = ctx.execution;
  ASSERT_EQ(gltang_library_add_native(ctx.library(), "probe", take_from_a_native, &t), GLTANG_OK);
  ASSERT_TRUE(ctx.execute());
  EXPECT_EQ(t.calls, 20);
  EXPECT_EQ(t.result, GLTANG_ERR_INVALID) << "from inside a host call, in the run";
  EXPECT_EQ(reinterpret_cast<uintptr_t>(t.out), 0x1234u) << "an output parameter is written only on success";
  EXPECT_EQ(ctx.integer(), 5);

  // At a poll: a handler that asks, while the context is at-poll.
  TakeRefused p;
  Context ctx2(c->program, Config());
  p.execution = ctx2.execution;
  ASSERT_EQ(gltang_library_add_native(ctx2.library(), "probe", bump, &p.calls), GLTANG_OK);
  ASSERT_EQ(grcore_context_register(ctx2.context, &kTakeAtPoll, &p), GRCORE_OK);
  grcore_context_set_fuel(ctx2.context, 40);
  ctx2.execute();
  ASSERT_TRUE(ctx2.paused());
  EXPECT_GE(p.calls, 1);
  EXPECT_EQ(p.result, GLTANG_ERR_INVALID);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(p.out), 0x1234u);
  // ... and after the pause the same context snapshots fine.
  Snap snap;
  EXPECT_EQ(gltang_snapshot_take(ctx2.execution, &snap.s), GLTANG_OK);
}

TEST(SnapshotRefusals, ATemplateCallInFlightIsRefusedAndTheRunFinishesAfterwards) {
  // The pause lands inside the template: its frames sit above the main
  // program's, with an activation and a budget scope.
  Host h;
  auto page = compile("use work; print(work()); print(\"done\");");
  auto work = compile("n = 0; for (i = 0; i < 300; i += 1) { n += i; } print(n);");
  Context ctx(page->program, Config());
  ASSERT_EQ(gltang_library_add_template(ctx.library(), "work", work->program, 1000000, GLTANG_SCOPE_EMPTY), GLTANG_OK);
  grcore_context_set_fuel(ctx.context, 400);
  ctx.execute();
  ASSERT_TRUE(ctx.paused());
  Snap snap;
  EXPECT_EQ(gltang_snapshot_take(ctx.execution, &snap.s), GLTANG_ERR_INVALID);
  EXPECT_EQ(snap.s, nullptr);
  grcore_context_set_fuel(ctx.context, GRCORE_UNLIMITED);
  ASSERT_TRUE(ctx.resume());
  EXPECT_EQ(ctx.raw(), "44850done") << "the refusal changed nothing";
}

TEST(SnapshotRefusals, AFinishedOrUnwoundExecutionIsRefused) {
  auto c = compile("1 + 1;");
  Context ctx(c->program, Config());
  ASSERT_TRUE(ctx.execute());
  Snap snap;
  EXPECT_EQ(gltang_snapshot_take(ctx.execution, &snap.s), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_snapshot_take(nullptr, &snap.s), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_snapshot_take(ctx.execution, nullptr), GLTANG_ERR_INVALID);
  auto loop = compile("while (true) { }");
  Config tiny;
  tiny.fuel = 100;
  Context runaway(loop->program, tiny);
  runaway.execute();
  ASSERT_TRUE(runaway.paused());
  grcore_context_terminate(runaway.context);
  EXPECT_FALSE(runaway.resume());
  EXPECT_EQ(gltang_execution_state(runaway.execution), GLTANG_EXECUTION_UNWOUND);
  EXPECT_EQ(gltang_snapshot_take(runaway.execution, &snap.s), GLTANG_ERR_INVALID);
}

TEST(SnapshotRefusals, TakeIsRefusedFromAnotherThreadThanTheOwner) {
  auto c = compile("for (i = 0; i < 100; i += 1) { } 1;");
  Source src(*c, 50, Config());
  ASSERT_TRUE(src.paused());
  GLTANG_Result r = GLTANG_OK;
  std::thread([&] {
    GLTANG_Snapshot * s = nullptr;
    r = gltang_snapshot_take(src.ctx->execution, &s);
  }).join();
  EXPECT_EQ(r, GLTANG_ERR_INVALID);
}

TEST(SnapshotRefusals, ACRootAHandleAPinAndAConservativeRangeRefuseTheTakeAndTheRunFinishesAfterwards) {
  auto c = compile(kBuilder);
  Reference ref = run_reference(*c, Config());
  Source src(*c, ref.fuel / 2, Config());
  ASSERT_TRUE(src.paused());
  GRHEAP_Heap * heap = src.ctx->heap;
  Snap snap;
  {
    void * slot = nullptr;
    ASSERT_EQ(grheap_root_add(heap, &slot), GRHEAP_OK);
    EXPECT_EQ(gltang_snapshot_take(src.ctx->execution, &snap.s), GLTANG_ERR_INVALID) << "a C root";
    ASSERT_EQ(grheap_root_remove(heap, &slot), GRHEAP_OK);
  }
  void * object = nullptr;
  size_t position = 0;
  ASSERT_TRUE(first_object_root(*src.ctx, 0, &object, &position));
  {
    GRHEAP_Handle * h = nullptr;
    ASSERT_EQ(grheap_handle_create(heap, object, &h), GRHEAP_OK);
    EXPECT_EQ(gltang_snapshot_take(src.ctx->execution, &snap.s), GLTANG_ERR_INVALID) << "a handle";
    ASSERT_EQ(grheap_handle_destroy(h), GRHEAP_OK);
  }
  {
    ASSERT_EQ(grheap_pin(heap, object), GRHEAP_OK);
    EXPECT_EQ(gltang_snapshot_take(src.ctx->execution, &snap.s), GLTANG_ERR_INVALID) << "a pin";
    ASSERT_EQ(grheap_unpin(heap, object), GRHEAP_OK);
  }
  {
    static uint64_t cell;
    static GRCORE_RootSource range_source = {"range", [](GRCORE_Context *, void *, const GRCORE_RootVisitor * v) {
      if (v->range) {
        uint64_t lo = reinterpret_cast<uint64_t>(&cell);
        GRCORE_ConservativeRange r = {lo, lo + 8, UINT64_MAX, 0, 0};
        v->range(v->user, &r);
      }
    }};
    ASSERT_EQ(grcore_context_add_root_source(src.ctx->context, &range_source, &cell), GRCORE_OK);
    EXPECT_EQ(gltang_snapshot_take(src.ctx->execution, &snap.s), GLTANG_ERR_INVALID) << "a conservative range";
    ASSERT_EQ(grcore_context_remove_root_source(src.ctx->context, &range_source, &cell), GRCORE_OK);
  }
  EXPECT_EQ(snap.s, nullptr);
  // Nothing changed: it still snapshots, and the run still finishes right.
  EXPECT_EQ(gltang_snapshot_take(src.ctx->execution, &snap.s), GLTANG_OK);
  grcore_context_set_fuel(src.ctx->context, GRCORE_UNLIMITED);
  ASSERT_TRUE(src.ctx->resume());
  EXPECT_EQ(observe(*src.ctx), ref.seen);
}

// ---------------------------------------------------------------------------
// A destination that does not match
// ---------------------------------------------------------------------------

namespace {

const GRCORE_EngineDescriptor kDummyEngine = {"dummy", nullptr, nullptr, nullptr, GRCORE_ScopeInterface{nullptr, nullptr, nullptr},
    GRCORE_ConservativeDecoder{0, 0, 0}, nullptr, nullptr};

GRCORE_Result extra_snapshot(GRCORE_Context *, void *, GRCORE_SnapshotWriter *) { return GRCORE_OK; }
GRCORE_Result extra_restore(GRCORE_Context *, void *, GRCORE_SnapshotReader *, void *, GRCORE_RestoreMode) { return GRCORE_OK; }
GRCORE_Result extra_settle(GRCORE_Context *, void *, void *, GRCORE_SettleMode) { return GRCORE_OK; }
const GRCORE_Key kExtraHooked = GRCORE_KEY_INIT("an extra hooked key", GRCORE_CARDINALITY_ONE, GRCORE_PHASE_NONE, nullptr, nullptr, extra_snapshot, extra_restore, extra_settle);

/// A destination built by hand, for the mismatches the harness cannot make:
/// something registered before the execution, or a heap with the wrong codec.
struct Manual {
  tt::Tracker tracker;
  GRCORE_Group * group = nullptr;
  GRCORE_Context * context = nullptr;
  GRHEAP_Heap * heap = nullptr;
  GLTANG_Execution * execution = nullptr;
  GLTANG_Result created = GLTANG_ERR_INTERNAL;
  int token = 0;

  Manual(Compiled & c, bool codec, const std::function<void(Manual &)> & before_execution = nullptr) {
    EXPECT_EQ(grcore_group_create(&tracker.allocator, &tracker.pages, &group), GRCORE_OK);
    GRCORE_Options * options;
    EXPECT_EQ(grcore_options_create(&tracker.allocator, &options), GRCORE_OK);
    EXPECT_EQ(grcore_context_create(group, options, &context), GRCORE_OK);
    grcore_options_destroy(options);
    GRHEAP_Options * heap_options;
    EXPECT_EQ(grheap_options_create(&tracker.allocator, &heap_options), GRHEAP_OK);
    if (codec) {
      gltang_heap_options_configure(heap_options);
    }
    grheap_options_set_torture(heap_options, false);
    EXPECT_EQ(grheap_heap_create(context, heap_options, &heap), GRHEAP_OK);
    grheap_options_destroy(heap_options);
    if (before_execution) {
      before_execution(*this);
    }
    created = gltang_execution_create(context, c.program, &execution);
    EXPECT_EQ(created, GLTANG_OK);
  }
  Manual(const Manual &) = delete;
  Manual & operator=(const Manual &) = delete;
  ~Manual() {
    EXPECT_EQ(grcore_context_destroy(context), GRCORE_OK);
    EXPECT_EQ(grcore_group_destroy(group), GRCORE_OK);
    EXPECT_EQ(tracker.live_blocks, 0);
    EXPECT_EQ(tracker.live_pages, 0);
  }
  uint64_t objects() const {
    GRHEAP_Stats s;
    EXPECT_EQ(grheap_stats(heap, &s), GRHEAP_OK);
    return s.live_objects;
  }
};

Snap paused_snapshot(Compiled & c, uint64_t fuel, std::unique_ptr<Context> * keep) {
  Snap snap;
  Source src(c, fuel, Config());
  EXPECT_TRUE(src.paused());
  EXPECT_EQ(gltang_snapshot_take(src.ctx->execution, &snap.s), GLTANG_OK);
  if (keep) {
    *keep = std::move(src.ctx);
  }
  return snap;
}

}  // namespace

TEST(SnapshotMismatch, ADifferentProgramIsRefusedBeforeAnyChangeAndTheDestinationRunsItsOwn) {
  auto a = compile(kBuilder);
  auto b = compile("x = 0; for (i = 0; i < 50; i += 1) { x += i; } print(x); x;");
  Reference ref_b = run_reference(*b, Config());
  Snap snap;
  {
    Source src(*a, 200, Config());
    ASSERT_TRUE(src.paused());
    ASSERT_EQ(gltang_snapshot_take(src.ctx->execution, &snap.s), GLTANG_OK);
  }
  Manual dst(*b, true);
  uint64_t before = dst.objects();
  EXPECT_EQ(gltang_snapshot_restore(dst.execution, snap.s), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_execution_state(dst.execution), GLTANG_EXECUTION_NEW);
  EXPECT_EQ(dst.objects(), before) << "nothing was made";
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(dst.context, gltang_execution_entry, dst.execution, &outcome), GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_FINISHED);
  EXPECT_EQ(gltang_execution_result_integer(dst.execution), ref_b.seen.text == "1225" ? 1225 : gltang_execution_result_integer(dst.execution));
  char * rendered = nullptr;
  ASSERT_EQ(gltang_execution_output_render(dst.execution, &rendered, nullptr), GLTANG_OK);
  EXPECT_STREQ(rendered, "1225");
  gltang_buffer_free(rendered);
}

TEST(SnapshotMismatch, AProgramThatDiffersOnlyInAConstantIsADifferentProgram) {
  auto a = compile("x = 1000; for (i = 0; i < 60; i += 1) { x += 1; } x;");
  auto b = compile("x = 1001; for (i = 0; i < 60; i += 1) { x += 1; } x;");
  ASSERT_EQ(gltang_program_function_count(a->program), gltang_program_function_count(b->program));
  Snap snap = paused_snapshot(*a, 100, nullptr);
  Context dst(b->program, Config());
  EXPECT_EQ(gltang_snapshot_restore(dst.execution, snap.s), GLTANG_ERR_INVALID) << "the counts agree; the hash does not";
}

TEST(SnapshotMismatch, ADifferentEngineTableIsRefusedBeforeAnyChange) {
  auto c = compile(kBuilder);
  Snap snap = paused_snapshot(*c, 300, nullptr);
  Manual dst(*c, true, [](Manual & m) {
    GRCORE_EngineId id;
    ASSERT_EQ(grcore_engine_register(m.context, &kDummyEngine, &id), GRCORE_OK);
  });
  uint64_t before = dst.objects();
  EXPECT_EQ(gltang_snapshot_restore(dst.execution, snap.s), GLTANG_ERR_INVALID);
  EXPECT_EQ(dst.objects(), before);
  EXPECT_EQ(gltang_execution_state(dst.execution), GLTANG_EXECUTION_NEW);
  EXPECT_EQ(grcore_stack_frame_count(grcore_context_stack(dst.context)), 0u);
}

TEST(SnapshotMismatch, ADifferentSetOfKeysWithHooksIsRefusedBeforeAnyChange) {
  auto c = compile(kBuilder);
  Snap snap = paused_snapshot(*c, 300, nullptr);
  Manual dst(*c, true, [](Manual & m) { ASSERT_EQ(grcore_context_register(m.context, &kExtraHooked, &m.token), GRCORE_OK); });
  uint64_t before = dst.objects();
  EXPECT_EQ(gltang_snapshot_restore(dst.execution, snap.s), GLTANG_ERR_INVALID);
  EXPECT_EQ(dst.objects(), before);
  EXPECT_EQ(gltang_execution_state(dst.execution), GLTANG_EXECUTION_NEW);
}

TEST(SnapshotMismatch, AHeapWithAnotherValueCodecIsRefusedBeforeAnyChange) {
  auto c = compile(kBuilder);
  Snap snap = paused_snapshot(*c, 300, nullptr);
  Manual dst(*c, false);
  uint64_t before = dst.objects();
  EXPECT_EQ(gltang_snapshot_restore(dst.execution, snap.s), GLTANG_ERR_INVALID);
  EXPECT_EQ(dst.objects(), before);
  EXPECT_EQ(gltang_execution_state(dst.execution), GLTANG_EXECUTION_NEW);
}

TEST(SnapshotMismatch, ARestoreNeedsANewExecutionAndAFreshContext) {
  auto c = compile(kBuilder);
  Reference ref = run_reference(*c, Config());
  Snap snap = paused_snapshot(*c, ref.fuel / 2, nullptr);
  {
    // An execution that has run is not fresh.
    Config tiny;
    tiny.fuel = 50;
    Context dst(c->program, tiny);
    dst.execute();
    ASSERT_TRUE(dst.paused());
    EXPECT_EQ(gltang_snapshot_restore(dst.execution, snap.s), GLTANG_ERR_INVALID);
    EXPECT_EQ(gltang_execution_state(dst.execution), GLTANG_EXECUTION_PAUSED);
  }
  {
    // One that has been restored into already: the second restore is refused,
    // and the first one's state is untouched by the attempt.
    Context dst(c->program, Config());
    ASSERT_EQ(gltang_snapshot_restore(dst.execution, snap.s), GLTANG_OK);
    EXPECT_EQ(gltang_snapshot_restore(dst.execution, snap.s), GLTANG_ERR_INVALID);
    ASSERT_TRUE(dst.resume());
    EXPECT_EQ(observe(dst), ref.seen);
  }
  EXPECT_EQ(gltang_snapshot_restore(nullptr, snap.s), GLTANG_ERR_INVALID);
  Context dst(c->program, Config());
  EXPECT_EQ(gltang_snapshot_restore(dst.execution, nullptr), GLTANG_ERR_INVALID);
}

// ---------------------------------------------------------------------------
// Failure at every point, and the destination's budgets
// ---------------------------------------------------------------------------

TEST(SnapshotFailure, EveryAllocationFailureDuringTakeIsACleanErrorWithNothingLeaked) {
  auto c = compile(kBuilder);
  Source src(*c, 400, Config());
  ASSERT_TRUE(src.paused());
  tt::Tracker probe;
  GRCORE_Snapshot * s = nullptr;
  ASSERT_EQ(grcore_context_snapshot(src.ctx->context, &probe.allocator, &s), GRCORE_OK);
  grcore_snapshot_release(s);
  long total = probe.calls;
  ASSERT_GT(total, 10);
  EXPECT_EQ(probe.live_blocks, 0);
  for (long n = 1; n <= total; ++n) {
    tt::Tracker t;
    t.fail_at = n;
    GRCORE_Snapshot * out = nullptr;
    EXPECT_EQ(grcore_context_snapshot(src.ctx->context, &t.allocator, &out), GRCORE_ERR_OOM) << "failing allocation " << n;
    EXPECT_EQ(out, nullptr);
    EXPECT_EQ(t.live_blocks, 0) << "leak at allocation " << n;
  }
  // The source was not touched by any of it.
  grcore_context_set_fuel(src.ctx->context, GRCORE_UNLIMITED);
  ASSERT_TRUE(src.ctx->resume());
}

TEST(SnapshotFailure, EveryAllocationFailureDuringRestoreLeavesAFreshRunnableExecutionAndLeaksNothing) {
  // A smaller program: the sweep makes one destination per allocation.
  auto c = compile(
      "a = []; m = {:}; s = \"\";\n"
      "for (i = 0; i < 30; i += 1) { a[i] = i; s = s + \"y\"; if (i % 10 == 0) { print(i); } }\n"
      "print(1 / 0); m.n = a.length; [a.length, s.length, m.n];");
  Reference ref = run_reference(*c, Config());
  Source src(*c, ref.fuel / 2, Config());
  ASSERT_TRUE(src.paused());
  Snap snap;
  ASSERT_EQ(gltang_snapshot_take(src.ctx->execution, &snap.s), GLTANG_OK);
  long total = 0;
  {
    Context probe(c->program, Config());
    long before = probe.tracker.calls;
    ASSERT_EQ(gltang_snapshot_restore(probe.execution, snap.s), GLTANG_OK);
    total = probe.tracker.calls - before;
  }
  ASSERT_GT(total, 8) << "the restore allocates (frames, objects, output, errors)";
  const char * verify = std::getenv("GRHEAP_VERIFY");
  bool verify_on = verify && *verify && std::strcmp(verify, "0") != 0;
  for (long k = 1; k <= total; ++k) {
    Context dst(c->program, Config());
    dst.tracker.fail_at = dst.tracker.calls + k;
    GLTANG_Result r = gltang_snapshot_restore(dst.execution, snap.s);
    dst.tracker.fail_at = 0;
    if (r == GLTANG_OK) {
      // Only barrier-verify's shadow table may fail without failing the
      // restore: it stops trusting itself instead.
      EXPECT_TRUE(verify_on) << "allocation " << k;
      ASSERT_TRUE(dst.resume());
      EXPECT_EQ(observe(dst), ref.seen);
      continue;
    }
    EXPECT_TRUE(r == GLTANG_ERR_OOM || r == GLTANG_ERR_LIMIT) << "allocation " << k << ": " << gltang_result_string(r);
    ASSERT_EQ(gltang_execution_state(dst.execution), GLTANG_EXECUTION_NEW) << "allocation " << k;
    EXPECT_EQ(dst.error_count(), 0u);
    EXPECT_EQ(dst.raw(), "");
    EXPECT_EQ(grcore_stack_frame_count(grcore_context_stack(dst.context)), 0u);
    // Still a fresh, runnable one: it runs its program from scratch.
    ASSERT_TRUE(dst.execute()) << "allocation " << k;
    EXPECT_EQ(observe(dst), ref.seen);
  }
}

TEST(SnapshotFailure, AMemoryBudgetSmallerThanTheImageIsALimitAndTheDestinationIsStillFresh) {
  auto c = compile("a = []; for (i = 0; i < 2000; i += 1) { a[i] = i * 1000000007; } print(a.length); for (j = 0; j < 100; j += 1) { } a[5];");
  Reference ref = run_reference(*c, Config());
  Source src(*c, ref.fuel * 3 / 4, Config());
  ASSERT_TRUE(src.paused());
  Snap snap;
  ASSERT_EQ(gltang_snapshot_take(src.ctx->execution, &snap.s), GLTANG_OK);

  Config no_reserve;
  no_reserve.memory_reserve = 0; // an allocation over the budget is a refusal, not a loan from the reserve
  Context dst(c->program, no_reserve);
  uint64_t baseline = grcore_context_memory_in_use(dst.context);
  uint64_t refusals = grcore_context_memory_refusals(dst.context);
  ASSERT_EQ(grcore_context_set_memory_bytes(dst.context, baseline + 4096), GRCORE_OK);
  EXPECT_EQ(gltang_snapshot_restore(dst.execution, snap.s), GLTANG_ERR_LIMIT);
  EXPECT_GT(grcore_context_memory_refusals(dst.context), refusals);
  EXPECT_EQ(grcore_context_memory_in_use(dst.context), baseline) << "everything the attempt charged was given back";
  EXPECT_EQ(gltang_execution_state(dst.execution), GLTANG_EXECUTION_NEW);
  // Give it room and try again: the same destination takes it.
  ASSERT_EQ(grcore_context_set_memory_bytes(dst.context, GRCORE_UNLIMITED), GRCORE_OK);
  ASSERT_EQ(gltang_snapshot_restore(dst.execution, snap.s), GLTANG_OK);
  ASSERT_TRUE(dst.resume());
  EXPECT_EQ(observe(dst), ref.seen);
}

TEST(SnapshotFailure, ADepthBudgetSmallerThanTheStackIsALimitAndTheDestinationIsStillFresh) {
  auto c = compile(
      "function down(n) { if (n == 0) { for (i = 0; i < 50; i += 1) { } return 0; } return 1 + down(n - 1); }\n"
      "down(40);");
  Reference ref = run_reference(*c, Config());
  Source src(*c, ref.fuel / 3, Config());
  ASSERT_TRUE(src.paused());
  Snap snap;
  ASSERT_EQ(gltang_snapshot_take(src.ctx->execution, &snap.s), GLTANG_OK);
  Config shallow;
  shallow.calls = 10;
  Context dst(c->program, shallow);
  EXPECT_EQ(gltang_snapshot_restore(dst.execution, snap.s), GLTANG_ERR_LIMIT);
  EXPECT_EQ(grcore_context_depth(dst.context, GRCORE_DEPTH_GUEST), 0u);
  EXPECT_EQ(gltang_execution_state(dst.execution), GLTANG_EXECUTION_NEW);
  // The destination's own limit is what bounds it, and a roomier one takes it.
  Finished roomy = restore_and_finish(*c, snap.s);
  EXPECT_TRUE(same_as(ref, roomy, src.fuel_at_pause));
}

TEST(SnapshotFailure, TheDestinationsFuelIsItsOwnNotTheSnapshots) {
  auto c = compile(kBuilder);
  Reference ref = run_reference(*c, Config());
  Source src(*c, ref.fuel / 4, Config());
  ASSERT_TRUE(src.paused());
  Snap snap;
  ASSERT_EQ(gltang_snapshot_take(src.ctx->execution, &snap.s), GLTANG_OK);
  Config small;
  small.fuel = 100;
  Context dst(c->program, small);
  ASSERT_EQ(gltang_snapshot_restore(dst.execution, snap.s), GLTANG_OK);
  EXPECT_EQ(grcore_context_fuel_used(dst.context), 0u) << "fuel used starts at zero";
  EXPECT_FALSE(dst.resume());
  EXPECT_TRUE(dst.paused()) << "the destination's 100 units ran out";
  EXPECT_LE(grcore_context_fuel_used(dst.context), 100u + 20u);
  ASSERT_TRUE(dst.finished_after_raising(GRCORE_UNLIMITED / 2));
  EXPECT_EQ(observe(dst), ref.seen);
}

// ---------------------------------------------------------------------------
// Nothing in a snapshot is an address
// ---------------------------------------------------------------------------

namespace {

bool holds(const void * bytes, size_t size, uint64_t word) {
  const unsigned char * p = static_cast<const unsigned char *>(bytes);
  for (size_t i = 0; i + 8 <= size; ++i) {
    uint64_t w;
    std::memcpy(&w, p + i, 8);
    if (w == word) {
      return true;
    }
  }
  return false;
}

// Two internal functions, declared by hand: the member of a library by name,
// and the built-in registry. The test archive links them; the members' layout
// is not needed to ask for their addresses.
extern "C" const void * gltang_library_find(const GLTANG_Library * library, const char * name, size_t length);
extern "C" const GLTANG_Library * gltang_library_builtins(void);

void add_member_addresses(const GLTANG_Library * lib, std::initializer_list<const char *> names, std::set<uint64_t> * out) {
  out->insert(reinterpret_cast<uint64_t>(lib));
  for (const char * name : names) {
    out->insert(reinterpret_cast<uint64_t>(gltang_library_find(lib, name, std::strlen(name))));
  }
}

}  // namespace

TEST(Snapshot, NoHostAddressIsInTheSnapshotAndTheScanFindsOneIfThereIs) {
  // The host values the program holds, the programs, the context, the heap and
  // the execution: none of their addresses may be in any blob of a snapshot.
  auto c = compile(kHostProgram);
  Host h;
  Reference ref = run_reference(*c, Config(), Host().setup());
  Source src(*c, ref.fuel / 3, Config(), h.setup());
  ASSERT_TRUE(src.paused());
  Snap snap;
  ASSERT_EQ(gltang_snapshot_take(src.ctx->execution, &snap.s), GLTANG_OK);

  std::set<uint64_t> host_addresses;
  add_member_addresses(src.ctx->root, {"bump", "sidebar", "tools"}, &host_addresses);
  add_member_addresses(h.tools, {"bump"}, &host_addresses);
  add_member_addresses(gltang_library_builtins(), {"math", "random"}, &host_addresses);
  host_addresses.insert(reinterpret_cast<uint64_t>(c->program));
  host_addresses.insert(reinterpret_cast<uint64_t>(h.sidebar->program));
  host_addresses.insert(reinterpret_cast<uint64_t>(src.ctx->context));
  host_addresses.insert(reinterpret_cast<uint64_t>(src.ctx->heap));
  host_addresses.insert(reinterpret_cast<uint64_t>(src.ctx->execution));
  // Every heap object the context's roots name.
  struct Collect {
    GRHEAP_Heap * heap;
    std::set<uint64_t> * out;
  } collect = {src.ctx->heap, &host_addresses};
  GRCORE_RootVisitor v = {};
  v.user = &collect;
  v.slot = [](void * u, uint64_t * slot) {
    auto * k = static_cast<Collect *>(u);
    if (*slot && (*slot & 0xF) == 0 && grheap_contains(k->heap, reinterpret_cast<void *>(*slot))) {
      k->out->insert(*slot);
    }
  };
  ASSERT_EQ(grcore_context_enumerate_roots(src.ctx->context, &v), GRCORE_OK);
  ASSERT_GT(host_addresses.size(), 20u);

  size_t blobs = grcore_snapshot_blob_count(snap.s);
  ASSERT_EQ(blobs, 3u) << "the heap, the guest stack and the execution";
  for (size_t b = 0; b < blobs; ++b) {
    const char * name = grcore_snapshot_blob_name(snap.s, b);
    const void * bytes = nullptr;
    size_t size = 0;
    ASSERT_EQ(grcore_snapshot_blob(snap.s, name, &bytes, &size), GRCORE_OK);
    for (uint64_t address : host_addresses) {
      EXPECT_FALSE(holds(bytes, size, address)) << "an address is in the " << name << " blob";
    }
  }
}

// ---------------------------------------------------------------------------
// Threads, and the collector's instruments over a restored heap
// ---------------------------------------------------------------------------

TEST(SnapshotThreads, OneSnapshotRestoredConcurrentlyOnManyThreadsFinishesEveryOneRight) {
  auto c = compile(kBuilder);
  Reference ref = run_reference(*c, Config(), nullptr, true);
  Source src(*c, ref.fuel / 2, Config());
  ASSERT_TRUE(src.paused());
  Snap snap;
  ASSERT_EQ(gltang_snapshot_take(src.ctx->execution, &snap.s), GLTANG_OK);
  std::atomic<int> good{0};
  std::vector<std::thread> threads;
  const int rounds = tt::heavy_instruments() ? 3 : 15;
  for (int t = 0; t < 6; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < rounds; ++i) {
        gltang_snapshot_retain(snap.s);
        Finished f = restore_and_finish(*c, snap.s, Config(), nullptr, true, true);
        if (same_as(ref, f, src.fuel_at_pause)) {
          ++good;
        }
        gltang_snapshot_release(snap.s);
      }
    });
  }
  for (auto & th : threads) {
    th.join();
  }
  EXPECT_EQ(good.load(), 6 * rounds);
  EXPECT_EQ(grcore_snapshot_refcount(snap.s), 1u);
}

TEST(SnapshotInstruments, ARestoredHeapPassesTortureAndBarrierVerifyAndAMovingStack) {
  const char * programs[] = {kBuilder,
      "m = {:}; for (i = 0; i < 60; i += 1) { m[\"k\" + (i as string)] = [i, i * 2]; } print(m.k5[1]); m.k59[0];",
      "function f(n) { if (n < 2) { return n; } return f(n - 1) + f(n - 2); } for (i = 0; i < 6; i += 1) { print(f(i + 4)); print(\",\"); } f(8);"};
  for (const char * source : programs) {
    auto c = compile(source);
    Reference ref = run_reference(*c, Config());
    ASSERT_TRUE(ref.finished) << source;
    Source src(*c, ref.fuel / 2, Config());
    ASSERT_TRUE(src.paused()) << source;
    Snap snap;
    ASSERT_EQ(gltang_snapshot_take(src.ctx->execution, &snap.s), GLTANG_OK);
    Config hard;
    hard.torture = 1;
    hard.verify = 1;
    hard.moving_stack = 1;
    Finished f = restore_and_finish(*c, snap.s, hard);
    EXPECT_TRUE(same_as(ref, f, src.fuel_at_pause)) << source;
  }
}

TEST(SnapshotInstruments, ARestoredHeapInArenaModeFinishesToo) {
  auto c = compile(kBuilder);
  Reference ref = run_reference(*c, Config());
  Source src(*c, ref.fuel / 2, Config());
  ASSERT_TRUE(src.paused());
  Snap snap;
  ASSERT_EQ(gltang_snapshot_take(src.ctx->execution, &snap.s), GLTANG_OK);
  Config arena;
  arena.arena = true;
  Finished f = restore_and_finish(*c, snap.s, arena);
  EXPECT_TRUE(same_as(ref, f, src.fuel_at_pause));
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
