// The baseline JIT (story 15): a function that is hot is compiled, entered
// right after its entry poll, and left through a guard or a return, and every
// observable thing - the result, the output, the error list, the fuel, the
// polls, the frames a debugger sees - is what the interpreter gives.
//
// Every test here is a differential: one scenario run with the JIT off
// (threshold 0) and on (threshold 1, so every function tiers up at its first
// poll), and the two runs compared. The stats say the run was not vacuous. A
// build with JIT=no compiles the JIT tests out and runs the one that says what
// is absent (the bottom of the file).

#include "exec_harness.h"
#include "fuzz/gen.h"
#include "test_helpers.h"

#include <algorithm>
#include <dirent.h>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <ghoti.io/runtime-core/runtime-core.h>

using tt::Compiled;
using tt::Config;
using tt::Context;
using tt::Mode;

extern "C" void gltang_vm_set_statement_polls_unchecked(GLTANG_Execution * execution, bool enabled);

namespace {

// ---------------------------------------------------------------------------
// A poll handler a test scripts: it runs at every poll (a request stays pending
// so that every poll takes the slow path, as the observer's does), counts them,
// and records the identity of each.
// ---------------------------------------------------------------------------

struct Script {
  std::function<void(Script &, GRCORE_Context *, uint64_t)> on_poll;
  uint64_t polls = 0;
  std::vector<GRCORE_PollIdentity> identities;
  GRCORE_Port * port = nullptr;
  GRCORE_RequestKind kind = 0;

  Script() = default;
  Script(const Script &) = delete;
  Script & operator=(const Script &) = delete;
  ~Script() { grcore_port_release(port); }

  static const GRCORE_Key & key() {
    static const GRCORE_Key k = {"jit test script", GRCORE_CARDINALITY_ONE, GRCORE_PHASE_ACT, nullptr, &Script::handler, nullptr, nullptr, nullptr};
    return k;
  }

  bool attach(GRCORE_Context * context) {
    if (grcore_context_request_kind(context, &key(), &kind) != GRCORE_OK || grcore_context_port(context, &port) != GRCORE_OK ||
        grcore_context_register(context, &key(), this) != GRCORE_OK) {
      return false;
    }
    return grcore_port_post(port, kind) == GRCORE_OK;
  }

  /// Posts a request of a core kind from inside a poll.
  void post(GRCORE_RequestKind k) { EXPECT_EQ(grcore_port_post(port, k), GRCORE_OK); }

 private:
  static void handler(GRCORE_Context * context, void * value, GRCORE_PollCall *) {
    Script * self = static_cast<Script *>(value);
    GRCORE_PollIdentity id = {0, 0};
    if (grcore_context_poll_identity(context, &id) == GRCORE_OK) {
      self->identities.push_back(id);
    }
    uint64_t n = self->polls++;
    if (self->on_poll) {
      self->on_poll(*self, context, n);
    }
  }
};

struct Part {
  std::string name, source;
  uint64_t fuel = 100000;
  GLTANG_ScopePolicy policy = GLTANG_SCOPE_EMPTY;
  Mode mode = Mode::Script;
};

/// What to run and how.
struct Scenario {
  std::string source;
  Mode mode = Mode::Script;
  std::vector<Part> parts;
  uint64_t fuel = GRCORE_UNLIMITED;      ///< The context's budget.
  uint64_t step = 0;                     ///< Nonzero: on a pause raise the budget by this and resume.
  bool statement_polls = false;
  int torture = -1, verify = -1, moving = -1;
  uint64_t memory_bytes = GRCORE_UNLIMITED;
  uint64_t memory_reserve = GRCORE_DEFAULT_MEMORY_RESERVE;
  uint64_t native_depth = GRCORE_UNLIMITED;
  bool fail_protect = false;
  bool resume = false;                   ///< On a pause with `step` 0, resume without raising the budget (an interrupt).
  bool script = false;                   ///< Attach a Script, with `on_poll` below.
  std::function<void(Script &, GRCORE_Context *, uint64_t)> on_poll;
  std::function<void(Context &)> before;  ///< Called after the context is made and before the run.
  size_t max_pauses = 4000;
};

/// Everything observable about a run.
struct Outcome {
  bool created = false;
  bool finished = false;
  GRCORE_Result ran = GRCORE_OK;
  std::string raw, rendered, result, errors;
  std::vector<std::string> pauses;       ///< For each pause: where, identity and fuel used.
  uint64_t fuel = 0;                     ///< Fuel used at the end.
  uint64_t polls = 0;
  std::vector<GRCORE_PollIdentity> identities;
  GLTANG_JitStats stats = {};
  uint64_t native_depth = 0;
  uint64_t memory_peak = 0;
  std::string key() const {
    std::string s = raw + "|" + rendered + "|" + result + "|" + errors + "|" + std::to_string(ran) + "|" + std::to_string(finished) + "|fuel " + std::to_string(fuel);
    for (const auto & p : pauses) {
      s += "|" + p;
    }
    return s;
  }
};

// run() and expect_same() serve the JIT arm only; the JIT=no arm has no test that
// calls them, and -Wunused-function would report them there.
#ifdef GLTANG_WITH_JIT
Outcome run(const Scenario & sc, long threshold) {
  Outcome out;
  Compiled page(sc.source, sc.mode, "jit.tang");
  EXPECT_TRUE(page.ok()) << page.error.message;
  if (!page.ok()) {
    return out;
  }
  Config config;
  config.fuel = sc.fuel;
  config.torture = sc.torture;
  config.verify = sc.verify;
  config.moving_stack = sc.moving;
  config.memory_bytes = sc.memory_bytes;
  config.memory_reserve = sc.memory_reserve;
  config.native_depth = sc.native_depth;
  config.jit_threshold = threshold;
  Context context(page.program, config);
  EXPECT_TRUE(context.ok());
  if (!context.ok()) {
    return out;
  }
  out.created = true;
  context.tracker.fail_protect = sc.fail_protect;
  EXPECT_EQ(gltang_execution_set_name(context.execution, "page"), GLTANG_OK);
  if (sc.statement_polls) {
    EXPECT_EQ(gltang_execution_set_statement_polls(context.execution, true), GLTANG_OK);
  }
  std::vector<std::unique_ptr<Compiled>> compiled;
  for (const Part & part : sc.parts) {
    compiled.push_back(std::make_unique<Compiled>(part.source, part.mode, (part.name + ".tang").c_str()));
    EXPECT_TRUE(compiled.back()->ok()) << part.name << ": " << compiled.back()->error.message;
    EXPECT_EQ(gltang_library_add_template(context.library(), part.name.c_str(), compiled.back()->program, part.fuel, part.policy), GLTANG_OK);
  }
  Script script;
  script.on_poll = sc.on_poll;
  if (sc.script) {
    EXPECT_TRUE(script.attach(context.context));
  }
  if (sc.before) {
    sc.before(context);
  }
  bool done = context.execute();
  while (!done && context.paused() && out.pauses.size() < sc.max_pauses) {
    GRCORE_Location where = grcore_context_pause_location(context.context);
    GRCORE_PollIdentity id = {0, 0};
    (void)grcore_context_poll_identity(context.context, &id);
    out.pauses.push_back(std::string(where.file ? where.file : "?") + ":" + std::to_string(where.line) + "@" + std::to_string(id.function) + "/" +
        std::to_string(id.offset) + " fuel " + std::to_string(grcore_context_fuel_used(context.context)));
    if (sc.step == 0) {
      if (sc.resume) {
        done = context.resume();
        continue;
      }
      break;
    }
    uint64_t used = grcore_context_fuel_used(context.context);
    grcore_context_set_fuel(context.context, used + sc.step);
    uint64_t depth = grcore_context_fuel_scope_depth(context.context);
    if (depth > 0) {
      uint64_t top = grcore_context_fuel_scope_top(context.context);
      uint64_t remaining = 1, scope_used = 0;
      if (grcore_context_fuel_scope_remaining(context.context, top, &remaining) == GRCORE_OK && remaining == 0 &&
          grcore_context_fuel_scope_used(context.context, top, &scope_used) == GRCORE_OK) {
        grcore_context_fuel_scope_set_budget(context.context, top, scope_used + sc.step);
      }
    }
    done = context.resume();
  }
  out.finished = done;
  out.ran = context.ran;
  out.raw = context.raw();
  out.rendered = context.rendered();
  out.result = context.describe();
  for (size_t i = 0; i < context.error_count(); ++i) {
    auto e = context.error(i);
    out.errors += e.template_name() + ":" + std::to_string(e.e.line) + "[" + e.chain_text() + "]" + std::to_string((int)e.e.how) + ":" + e.message() + ";";
  }
  out.fuel = grcore_context_fuel_used(context.context);
  out.polls = script.polls;
  out.identities = script.identities;
  out.stats = context.jit_stats();
  out.native_depth = grcore_context_depth(context.context, GRCORE_DEPTH_NATIVE);
  out.memory_peak = grcore_context_memory_peak(context.context);
  return out;
}

/// The program is run with the JIT off and with every function tiering up at
/// its first poll; the two must be the same run in every observable way.
void expect_same(const Scenario & sc, Outcome * interpreter = nullptr, Outcome * jit = nullptr) {
  Outcome a = run(sc, 0);
  Outcome b = run(sc, 1);
  ASSERT_TRUE(a.created);
  ASSERT_TRUE(b.created);
  EXPECT_EQ(a.key(), b.key());
  EXPECT_EQ(a.polls, b.polls);
  ASSERT_EQ(a.identities.size(), b.identities.size());
  for (size_t i = 0; i < a.identities.size(); ++i) {
    ASSERT_TRUE(a.identities[i].function == b.identities[i].function && a.identities[i].offset == b.identities[i].offset)
        << "poll " << i << ": " << a.identities[i].function << "/" << a.identities[i].offset << " against " << b.identities[i].function << "/"
        << b.identities[i].offset;
  }
  if (interpreter) {
    *interpreter = a;
  }
  if (jit) {
    *jit = b;
  }
}
#endif

const char * const kLoop =
    "function sum(n) { s = 0; i = 0; while (i < n) { s = s + i; i = i + 1; } return s; }\n"
    "print(sum(10)); print(sum(100)); print(sum(1000));\n";

}  // namespace

#ifdef GLTANG_WITH_JIT

namespace {
// The frame header: function, pc, sp, flags. Only the JIT's frame tests read
// it, so it is declared inside their guard (clang rejects an unused one).
constexpr size_t kHeader = 4;
}  // namespace

// ---------------------------------------------------------------------------
// Tier-up
// ---------------------------------------------------------------------------

TEST(Jit, ABuiltJitSaysSo) {
  EXPECT_TRUE(gltang_jit_built());
}

TEST(Jit, AHotLoopTiersUpAndGivesTheInterpretersOutput) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc;
  sc.source = kLoop;
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  EXPECT_EQ(plain.raw, "454950499500");
  EXPECT_TRUE(jit.finished);
  EXPECT_EQ(plain.stats.functions_compiled, 0u);
  EXPECT_EQ(plain.stats.entries, 0u);
  EXPECT_GE(jit.stats.functions_compiled, 1u);
  EXPECT_GE(jit.stats.entries, 3u);
  EXPECT_GE(jit.stats.returns, 3u) << "the three calls of sum return from compiled code";
  EXPECT_EQ(jit.stats.compile_failures, 0u);
  EXPECT_EQ(jit.stats.slow_polls, 0u) << "nothing was pending";
}

TEST(Jit, ThresholdOneContinuesTheSameInvocationInCompiledCode) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // One call, so the invocation that crossed the threshold is the only one:
  // it is the compiled code that ran the loop.
  Scenario sc;
  sc.source = "function f(n) { s = 0; i = 0; while (i < n) { s = s + 2 * i; i = i + 1; } return s; }\nprint(f(50));";
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  EXPECT_EQ(jit.raw, "2450");
  // The top level is entered too (function 0, which leaves compiled code at its
  // first call); f is the call that returns.
  EXPECT_GE(jit.stats.entries, 1u);
  EXPECT_EQ(jit.stats.returns, 1u);
}

TEST(Jit, AThresholdOfTwoHundredPollsCompilesAfterTwoHundredAndZeroMeansNever) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // With the library's default (the constant) a function compiles after 200
  // polls: a loop of 50 iterations does not reach it, one of 500 does.
  if (GLTANG_JIT_DEFAULT_THRESHOLD != 200u) {
    GTEST_SKIP() << "a build that overrides GLTANG_JIT_DEFAULT_THRESHOLD has another default than the 200 this test assumes";
  }
  auto source = [](int n) {
    return "function f(n) { s = 0; i = 0; while (i < n) { s = s + 1; i = i + 1; } return s; }\nprint(f(3)); print(f(" + std::to_string(n) + ")); print(f(3));";
  };
  Scenario small;
  small.source = source(50);
  Outcome a = run(small, GLTANG_JIT_DEFAULT_THRESHOLD);
  EXPECT_EQ(a.stats.functions_compiled, 0u);
  Scenario large;
  large.source = source(500);
  Outcome b = run(large, GLTANG_JIT_DEFAULT_THRESHOLD);
  EXPECT_GE(b.stats.functions_compiled, 1u);
  EXPECT_GE(b.stats.entries, 1u) << "the third call enters the code the second one compiled";
  EXPECT_EQ(b.raw, "3" "500" "3");
  Outcome off = run(large, 0);
  EXPECT_EQ(off.stats.functions_compiled, 0u);
  EXPECT_EQ(off.raw, b.raw);
}

TEST(Jit, AnExecutionThatIsGivenNoThresholdHasTheLibrarysDefault) {
  GLTANG_REQUIRE_JIT_BACKEND();
  if (tt::jit_threshold_requested() >= 0) {
    GTEST_SKIP() << "GLTANG_TEST_JIT_THRESHOLD overrides the default for every harness-made execution";
  }
  Scenario large;
  large.source = "function f(n) { s = 0; i = 0; while (i < n) { s = s + 1; i = i + 1; } return s; }\nprint(f(3)); print(f(500)); print(f(3));";
  Outcome b = run(large, -1);
  EXPECT_GE(b.stats.functions_compiled, 1u) << "500 iterations are more than the default of 200 polls";
  Scenario small;
  small.source = "function f(n) { s = 0; i = 0; while (i < n) { s = s + 1; i = i + 1; } return s; }\nprint(f(3)); print(f(50)); print(f(3));";
  EXPECT_EQ(run(small, -1).stats.functions_compiled, 0u);
}

TEST(Jit, TheThresholdIsRefusedOnceTheRunHasStarted) {
  Compiled page("print(1);", Mode::Script, "x.tang");
  ASSERT_TRUE(page.ok());
  Config config;
  config.jit_threshold = 0;
  Context context(page.program, config);
  ASSERT_TRUE(context.ok());
  EXPECT_EQ(gltang_execution_set_jit_threshold(context.execution, 7), GLTANG_OK);
  EXPECT_EQ(gltang_execution_set_jit_threshold(nullptr, 7), GLTANG_ERR_INVALID);
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(gltang_execution_set_jit_threshold(context.execution, 7), GLTANG_ERR_INVALID);
  GLTANG_JitStats stats;
  EXPECT_EQ(gltang_execution_jit_stats(nullptr, &stats), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_execution_jit_stats(context.execution, nullptr), GLTANG_ERR_INVALID);
}

// ---------------------------------------------------------------------------
// Guards and deoptimization
// ---------------------------------------------------------------------------

TEST(Jit, IntegerOverflowInsideCompiledCodeDeoptimizesAndTheInterpreterBoxesTheResult) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc;
  sc.source =
      "function add(a, b) { return a + b; }\n"
      "function sub(a, b) { return a - b; }\n"
      "function mul(a, b) { return a * b; }\n"
      "function neg(a) { return -a; }\n"
      "M = 576460752303423487;\n"
      "print(add(M, 1)); print(add(M, 0)); print(add(1, 2)); print(sub(-M - 1, 1)); print(sub(5, 7));\n"
      "print(mul(M, 2)); print(mul(536870911, 536870911)); print(mul(536870912, 2)); print(mul(-536870912, 536870911)); print(mul(3, 4));\n"
      "print(neg(-M - 1)); print(neg(7));\n";
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  EXPECT_NE(plain.raw.find("576460752303423488"), std::string::npos) << plain.raw;
  EXPECT_GT(jit.stats.deopts, 4u);
  EXPECT_GE(jit.stats.entries, 12u);
}

TEST(Jit, AnOperandThatIsNotASmallIntegerDeoptimizesAtTheOperation) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc;
  sc.source =
      "function add(a, b) { return a + b; }\n"
      "function lt(a, b) { return a < b; }\n"
      "function not(a) { return !a; }\n"
      "function eq(a, b) { return a == b; }\n"
      "x = add(1, 2); y = lt(1, 2); z = not(true); w = eq(3, 3);\n"
      "print(x); print(y); print(z); print(w);\n"
      "print(add(\"a\", \"b\")); print(add(1.5, 2)); print(add(2, 1.5)); print(add(576460752303423488, 1)); print(add(1, 576460752303423488));\n"
      "print(lt(\"a\", \"b\")); print(lt(1.5, 2)); print(lt(null, 1)); print(not(0)); print(not(\"s\")); print(not(null)); print(eq(\"a\", \"a\")); print(eq(1, 1.0));\n"
      "e = add(null, 1); print(e as string);\n"
      "f = add([1], 2); print(f as string);\n";
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  EXPECT_GE(jit.stats.deopts, 10u);
  EXPECT_EQ(plain.errors, jit.errors);
}

TEST(Jit, AnOperationCompiledCodeDoesNotInlineIsAnUnconditionalExitAndTheRestRunsInTheInterpreter) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc;
  sc.source =
      "function g(x) { return x * 2; }\n"
      "function f(n) { a = [1, 2, 3]; s = 0; i = 0; while (i < n) { s = s + g(i) + a[i % 3]; i = i + 1; } print(s); return s / 2 + s % 7; }\n"
      "print(f(20)); print(f(3)); print(f(0));\n"
      "function h(x) { return x / 0; }\nprint(h(4) as string);\n";
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  EXPECT_GE(jit.stats.deopts, 4u);
}

TEST(Jit, AFunctionWhoseBodyStartsWithAnOperationCompiledCodeLeavesIsStillCompiledBecauseALineComesFirst) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // Every statement begins with LINE, which is inline, so the "first operation
  // after the entry poll leaves compiled code" decline cannot be reached from
  // source: both functions are compiled, and the difference is where they leave.
  Scenario exits;
  exits.source = "function f(a) { return [a, a]; }\nprint(f(1) as string);\n";
  Scenario inlined;
  inlined.source = "function f(a) { return a + 1; }\nprint(f(1) as string);\n";
  Outcome e_plain, e_jit, i_plain, i_jit;
  expect_same(exits, &e_plain, &e_jit);
  expect_same(inlined, &i_plain, &i_jit);
  EXPECT_EQ(e_jit.stats.functions_compiled, 2u) << "the top level and f";
  EXPECT_EQ(e_jit.stats.entries, 2u);
  EXPECT_EQ(e_jit.stats.returns, 0u) << "f leaves at the array";
  EXPECT_EQ(i_jit.stats.functions_compiled, 2u);
  EXPECT_EQ(i_jit.stats.entries, 2u);
  EXPECT_EQ(i_jit.stats.returns, 1u) << "f returns from compiled code";
  EXPECT_EQ(e_jit.stats.compile_failures + i_jit.stats.compile_failures, 0u);
}

TEST(Jit, AFunctionThatDeoptimizesEightTimesIsDiscardedAndNeverCompiledAgain) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc;
  sc.source = "function f(a) { return a + 1; }\nfor (i = 0; i < 14; i += 1) { f(\"x\"); }\nprint(f(2));\n";
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  EXPECT_EQ(jit.stats.functions_discarded, 1u);
  // Eight deoptimizations of f, and then the interpreter alone; the top level
  // deoptimizes once, where it first calls f.
  EXPECT_EQ(jit.stats.deopts, 9u);
  EXPECT_EQ(jit.stats.entries, 9u);
  EXPECT_EQ(jit.raw, "3");
}

// ---------------------------------------------------------------------------
// Polls, pauses, unwinds and fuel
// ---------------------------------------------------------------------------

TEST(Jit, ACompiledLoopWithNothingPendingNeverCallsThePollHelperAndStillChargesFuel) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc;
  sc.source = "function f(n) { i = 0; while (i < n) { i = i + 1; } return i; }\nprint(f(20000));";
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  EXPECT_EQ(jit.stats.slow_polls, 0u);
  EXPECT_GE(jit.stats.entries, 1u);
  EXPECT_EQ(jit.fuel, plain.fuel);
  EXPECT_GT(jit.fuel, 20000u);
}

TEST(Jit, AFuelBudgetThatRunsOutInsideCompiledCodePausesAtTheSamePollWithTheSameFuelAndResumes) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc;
  sc.source = "function f(n) { s = 0; i = 0; while (i < n) { s = s + i; i = i + 1; } return s; }\nprint(f(300)); print(f(40));";
  sc.fuel = 100;
  sc.step = 100;
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  ASSERT_GT(plain.pauses.size(), 5u);
  EXPECT_EQ(plain.pauses, jit.pauses) << "the same poll identity, location and fuel at every pause";
  // A pause in compiled code leaves the rest of that invocation to the
  // interpreter, so there is one such pause for each of the two calls.
  EXPECT_GE(jit.stats.refused_pauses, 2u) << "pauses were taken inside compiled code";
  EXPECT_TRUE(jit.finished);
  // And what it prints is what the same program prints without a budget.
  Scenario free_run;
  free_run.source = sc.source;
  EXPECT_EQ(run(free_run, 1).raw, jit.raw);
  EXPECT_EQ(jit.raw, "44850" "780");
}

TEST(Jit, AnInterruptAtACompiledPollPausesAtTheRightFileAndLineAndResumeCompletes) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc;
  sc.source = "function f(n) {\n  s = 0;\n  i = 0;\n  while (i < n) {\n    s = s + i;\n    i = i + 1;\n  }\n  return s;\n}\nprint(f(2000));";
  sc.script = true;
  sc.resume = true;
  sc.on_poll = [](Script & script, GRCORE_Context *, uint64_t n) {
    if (n == 40) {
      script.post(GRCORE_REQUEST_INTERRUPT);
    }
  };
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  ASSERT_EQ(plain.pauses.size(), 1u);
  EXPECT_EQ(plain.pauses, jit.pauses);
  EXPECT_NE(jit.pauses[0].find("jit.tang:4@"), std::string::npos) << jit.pauses[0];
  EXPECT_EQ(jit.stats.refused_pauses, 1u);
  EXPECT_EQ(jit.raw, "1999000");
}

TEST(Jit, ATerminateRequestUnwindsAtACompiledPollLikeTheInterpretersOwn) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc;
  sc.source = "function f(n) { i = 0; while (i < n) { i = i + 1; } return i; }\nprint(f(100000));";
  sc.script = true;
  sc.on_poll = [](Script & script, GRCORE_Context *, uint64_t n) {
    if (n == 25) {
      script.post(GRCORE_REQUEST_TERMINATE);
    }
  };
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  EXPECT_FALSE(jit.finished);
  EXPECT_NE(jit.ran, GRCORE_OK);
  EXPECT_EQ(jit.stats.refused_unwinds, 1u);
}

TEST(Jit, ATemplateScopeThatRunsOutInsideCompiledCodeUnwindsWithTheScopeErrorAndTheErrorListEntry) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc;
  sc.source = "use t; print(\"[\" + t() + \"]\"); print(\"after\");";
  Part t;
  t.name = "t";
  t.source = "function spin(n) { k = 0; while (k < n) { k += 1; } return k; }\nprint(spin(100000));";
  t.fuel = 600;
  t.policy = GLTANG_SCOPE_EMPTY;
  sc.parts.push_back(t);
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  EXPECT_GE(jit.stats.refused_unwinds, 1u);
  EXPECT_FALSE(jit.errors.empty()) << "the scope's stop is in the error list";
  EXPECT_NE(jit.raw.find("after"), std::string::npos);
}

TEST(Jit, FuelIsTheSameOnEveryTierForTheSameProgramAndBudget) {
  GLTANG_REQUIRE_JIT_BACKEND();
  const char * programs[] = {
      kLoop,
      "function f(a, b) { if (a < b) { return a * b; } return a - b; }\ns = 0; for (i = 0; i < 40; i += 1) { s = s + f(i, 20); } print(s);",
      "function fib(n) { if (n < 2) { return n; } return fib(n - 1) + fib(n - 2); }\nprint(fib(12));",
      "function f(n) { b = true; i = 0; while (i < n) { b = !b; if (b) { i = i + 2; } else { i = i + 1; } } return i; }\nprint(f(77));",
  };
  for (const char * source : programs) {
    for (uint64_t budget : {uint64_t{37}, uint64_t{250}, uint64_t{999}}) {
      Scenario sc;
      sc.source = source;
      sc.fuel = budget;
      sc.step = budget;
      Outcome plain, jit;
      expect_same(sc, &plain, &jit);
      EXPECT_EQ(plain.fuel, jit.fuel) << source << " with budget " << budget;
      EXPECT_EQ(plain.pauses, jit.pauses) << source << " with budget " << budget;
    }
  }
}

TEST(Jit, StatementPollsInCompiledCodeAreTheInterpretersAndAreReadAtEveryLineNotWhenCompiled) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc;
  sc.source = "function f(n) {\n  s = 0;\n  i = 0;\n  while (i < n) {\n    s = s + i;\n    i = i + 1;\n  }\n  return s;\n}\nprint(f(30));\nprint(f(30));";
  sc.statement_polls = true;
  sc.script = true;
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  Scenario off = sc;
  off.statement_polls = false;
  Outcome without = run(off, 1);
  EXPECT_GT(jit.polls, without.polls) << "statement polls add polls inside compiled code";
  EXPECT_EQ(jit.raw, without.raw);
}

TEST(Jit, ASwitchFlippedWhileCompiledCodeRunsTakesEffectAtTheNextLineAndCostsNothingWhenClear) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // The execution's statement-poll switch is turned on at the tenth poll and off
  // at the fortieth by the poll handler, while the compiled loop runs. Both tiers
  // must poll at the same places: the compiled LINE reads the switch every time.
  Scenario sc;
  sc.source = "function f(n) {\n  s = 0;\n  i = 0;\n  while (i < n) {\n    s = s + i;\n    i = i + 1;\n  }\n  return s;\n}\nprint(f(60));";
  sc.script = true;
  GLTANG_Execution * target = nullptr;
  sc.before = [&target](Context & context) { target = context.execution; };
  sc.on_poll = [&target](Script &, GRCORE_Context *, uint64_t n) {
    if (n == 10) {
      gltang_vm_set_statement_polls_unchecked(target, true);
    }
    if (n == 40) {
      gltang_vm_set_statement_polls_unchecked(target, false);
    }
  };
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  // The never-flipped run has 61 polls (one per iteration and the exit).
  Scenario fixed;
  fixed.source = sc.source;
  fixed.script = true;
  Outcome never = run(fixed, 1);
  EXPECT_GT(plain.polls, never.polls) << "the switch added polls while it was on";
  EXPECT_LT(plain.polls, never.polls + 2 * 30 + 3) << "and stopped adding them when it was cleared";
  EXPECT_EQ(plain.raw, never.raw);
}

TEST(Jit, AMissedWriteBackAtAPollWouldLoseWhatTheCollectorDidToASlot) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // A collector that moves objects updates the references in the guest frame,
  // and the compiled code that continues must see them. The poll handler plays
  // that collector: it overwrites a local in the top frame at one poll. If the
  // compiled code kept its stale register the program would print another sum.
  Scenario sc;
  sc.source = "function f(n) { x = 0; i = 0; while (i < n) { x = x + 1; i = i + 1; } return x; }\nprint(f(30));";
  sc.script = true;
  sc.on_poll = [](Script &, GRCORE_Context * context, uint64_t n) {
    if (n != 12) {
      return;
    }
    GRCORE_Stack * stack = grcore_context_stack(context);
    GRCORE_FrameRef top = grcore_stack_top(stack);
    // f's locals: n, x, i (parameters first); the header is four words.
    uint64_t * slots = grcore_stack_slots(stack, top);
    ASSERT_NE(slots, nullptr);
    slots[kHeader + 1] = (uint64_t{1000} << 4) | 1u;  // x = 1000
  };
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  EXPECT_NE(plain.raw, "30") << "the overwrite changed the answer in the interpreter";
  EXPECT_EQ(jit.raw, plain.raw) << "and in compiled code";
  EXPECT_GE(jit.stats.slow_polls, 12u);
}

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------

TEST(Jit, ARefusedNativeDepthMeansTheFunctionIsNotEnteredAndTheBudgetIsUntouched) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc;
  sc.source = kLoop;
  sc.native_depth = 0;
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  EXPECT_GE(jit.stats.functions_compiled, 1u);
  EXPECT_EQ(jit.stats.entries, 0u);
  EXPECT_EQ(jit.native_depth, 0u);
  // With room, the same program enters.
  Scenario room;
  room.source = kLoop;
  room.native_depth = 4;
  Outcome with_room = run(room, 1);
  EXPECT_GE(with_room.stats.entries, 3u);
  EXPECT_EQ(with_room.native_depth, 0u) << "every record was left";
}

TEST(Jit, CodeMemoryThatTheBudgetRefusesMarksTheFunctionNeverCompileAndTheRunContinues) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc;
  sc.source = kLoop;
  // The most the interpreter needs, measured with the JIT counting but never
  // compiling, and a budget a page short of what compiling needs.
  Outcome measured = run(sc, 1000000);
  ASSERT_TRUE(measured.created);
  Scenario tight = sc;
  tight.memory_reserve = 0;
  tight.memory_bytes = measured.memory_peak + 1024;
  Outcome plain = run(tight, 0);
  Outcome jit = run(tight, 1);
  EXPECT_EQ(plain.raw, "454950499500") << "the interpreter fits the budget";
  EXPECT_EQ(jit.raw, plain.raw);
  EXPECT_GE(jit.stats.compile_failures, 1u);
  EXPECT_EQ(jit.stats.functions_compiled, 0u);
  EXPECT_EQ(jit.stats.entries, 0u);
}

TEST(Jit, APageProviderThatCannotProtectRefusesTheCompileAndTheRunContinues) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc;
  sc.source = kLoop;
  sc.fail_protect = true;
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  EXPECT_GE(jit.stats.compile_failures, 1u);
  EXPECT_EQ(jit.stats.entries, 0u);
}

namespace {

std::string function_with_locals(int locals) {
  std::string source = "function f(a) {\n";
  for (int i = 0; i < locals; ++i) {
    source += "  v" + std::to_string(i) + " = a + " + std::to_string(i) + ";\n";
  }
  source += "  return v" + std::to_string(locals - 1) + " + v0;\n}\nprint(f(1)); print(f(2));";
  return source;
}

}  // namespace

TEST(Jit, AFunctionWithManyLocalsIsCompiledAndOneWithTooManySlotsIsDeclinedWithoutHarm) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // 300 locals fit the 1,024-slot limit: the top level and f are compiled.
  Scenario tall;
  tall.source = function_with_locals(300);
  Outcome plain, jit;
  expect_same(tall, &plain, &jit);
  EXPECT_EQ(jit.stats.functions_compiled, 2u);
  EXPECT_EQ(jit.stats.compile_failures, 0u);
  EXPECT_GE(jit.stats.returns, 2u) << "both calls of f return from compiled code";

  // 1,100 locals make more than 1,023 frame slots: f is declined (not a failure,
  // not compiled), only the top level is, and the run is the interpreter's.
  Scenario huge;
  huge.source = function_with_locals(1100);
  expect_same(huge, &plain, &jit);
  EXPECT_EQ(jit.stats.functions_compiled, 1u) << "the top level only: f is over the slot limit";
  EXPECT_EQ(jit.stats.compile_failures, 0u);
  EXPECT_EQ(jit.stats.returns, 0u);
}

// ---------------------------------------------------------------------------
// The other instruments
// ---------------------------------------------------------------------------

TEST(Jit, AMovingGuestStackIsReloadedAfterEveryPollInsideCompiledCode) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc;
  sc.source = "function f(n) { s = 0; i = 0; while (i < n) { s = s + i; i = i + 1; } return s; }\nprint(f(200)); print(f(50));";
  sc.script = true;
  sc.moving = 1;
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  EXPECT_GT(jit.stats.slow_polls, 100u);
  EXPECT_EQ(jit.raw, "19900" "1225");
}

TEST(Jit, ACollectionAtACompiledPollSeesEveryReferenceInTheGuestFrame) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // The references are arguments, so they are in registers of compiled code and
  // in the guest frame the poll wrote; the handler collects at some polls, and
  // the program must still read them afterwards.
  Scenario sc;
  sc.source =
      "function f(arr, str, n) { i = 0; while (i < n) { i = i + 1; } return i; }\n"
      "a = [\"x\" + 1, \"y\" + 2]; s = \"hello\" + \"world\";\n"
      "print(f(a, s, 60)); print(a[0]); print(a[1]); print(s);\n";
  sc.script = true;
  sc.torture = 1;
  sc.verify = 1;
  sc.on_poll = [](Script &, GRCORE_Context * context, uint64_t n) {
    if (n % 7 == 3) {
      EXPECT_EQ(grheap_collect(grheap_heap_get(context)), GRHEAP_OK);
    }
  };
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  EXPECT_EQ(jit.raw, "60x1y2helloworld");
  EXPECT_GT(jit.stats.slow_polls, 10u);
}

TEST(Jit, AContextPausedInsideCompiledCodeResumesOnAnotherThreadWithTheSameOutput) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // The compiled code is gone from the native stack at a pause (the guest frame
  // holds everything), so the context can move to another thread, and every hop
  // that enters a function again enters compiled code on its own thread.
  const char * source =
      "function tri(n) { s = 0; i = 1; while (i <= n) { s = s + i; i = i + 1; } return s; }\n"
      "out = []; for (k = 1; k <= 40; k += 1) { out[k - 1] = tri(k); print(tri(k) % 7); }\nout;";
  Compiled compiled(source, Mode::Script, "hop.tang");
  ASSERT_TRUE(compiled.ok());
  Config reference_config;
  reference_config.jit_threshold = 0;
  Context reference(compiled.program, reference_config);
  ASSERT_TRUE(reference.ok());
  ASSERT_TRUE(reference.execute());
  Config config;
  config.fuel = 300;
  config.jit_threshold = 1;
  Context context(compiled.program, config);
  ASSERT_TRUE(context.ok());
  ASSERT_FALSE(context.execute());
  ASSERT_TRUE(context.paused());
  int hops = 0;
  bool finished = false;
  while (!finished) {
    ASSERT_EQ(grcore_context_release(context.context), GRCORE_OK);
    GRCORE_Result acquired = GRCORE_ERR_INTERNAL;
    std::thread t([&]() {
      acquired = grcore_context_acquire(context.context);
      if (acquired != GRCORE_OK) {
        return;
      }
      grcore_context_set_fuel(context.context, grcore_context_fuel_used(context.context) + 350);
      finished = context.resume();
      grcore_context_release(context.context);
    });
    t.join();
    ASSERT_EQ(acquired, GRCORE_OK);
    ASSERT_EQ(grcore_context_acquire(context.context), GRCORE_OK);
    ASSERT_LT(++hops, 500);
  }
  EXPECT_GT(hops, 2);
  EXPECT_EQ(context.raw(), reference.raw());
  EXPECT_EQ(context.describe(), reference.describe());
  EXPECT_GT(context.jit_stats().refused_pauses, 2u) << "pauses were taken inside compiled code";
  EXPECT_GT(context.jit_stats().entries, 40u) << "and later calls entered compiled code on the other threads";
}

// ---------------------------------------------------------------------------
// The corpus and generated programs, JIT off against threshold one
// ---------------------------------------------------------------------------

namespace {

std::vector<std::string> tang_files(const std::string & sub) {
  std::vector<std::string> names;
  std::string dir = std::string(GLTANG_TEST_DATA) + "/corpus/" + sub;
  if (DIR * d = opendir(dir.c_str())) {
    while (dirent * e = readdir(d)) {
      std::string n = e->d_name;
      if (n.size() > 5 && n.compare(n.size() - 5, 5, ".tang") == 0) {
        names.push_back(dir + "/" + n);
      }
    }
    closedir(d);
  }
  std::sort(names.begin(), names.end());
  return names;
}

}  // namespace

TEST(Jit, TheCorpusRunsToTheSameVerdictWithEveryFunctionCompiledAtItsFirstPoll) {
  GLTANG_REQUIRE_JIT_BACKEND();
  uint64_t entries = 0, programs = 0, deopts = 0, compared = 0;
  for (const char * sub : {"script", "template"}) {
    for (const std::string & path : tang_files(sub)) {
      if (path.find("reject") != std::string::npos || path.find("container-nested") != std::string::npos ||
          path.find("heavy") != std::string::npos) {
        continue;
      }
      std::string source = read_file(path);
      if (source.size() > 2500) {
        continue;
      }
      Scenario sc;
      sc.source = source;
      sc.mode = std::string(sub) == "script" ? Mode::Script : Mode::Template;
      sc.fuel = 100000;
      Compiled probe(source, sc.mode, "probe.tang");
      if (!probe.ok()) {
        continue;
      }
      Outcome a = run(sc, 0);
      Outcome b = run(sc, 1);
      ASSERT_TRUE(a.created && b.created) << path;
      EXPECT_EQ(a.key(), b.key()) << path;
      entries += b.stats.entries;
      deopts += b.stats.deopts;
      ++compared;
      programs += b.stats.functions_compiled > 0;
    }
  }
  EXPECT_GT(compared, 300u);
  EXPECT_GT(programs, 100u) << "most programs compile something";
  EXPECT_GT(entries, 200u) << "compiled code ran: the comparison is not vacuous";
  EXPECT_GT(deopts, 100u);
}

TEST(Jit, GeneratedProgramsRunToTheSameVerdictWithEveryFunctionCompiledAtItsFirstPoll) {
  GLTANG_REQUIRE_JIT_BACKEND();
  uint64_t entries = 0, compared = 0;
  for (uint64_t seed = 1; seed <= 60; ++seed) {
    for (gen::Mode mode : {gen::Mode::Script, gen::Mode::Template}) {
      Scenario sc;
      sc.source = gen::generate(seed, mode).source;
      sc.mode = mode == gen::Mode::Script ? Mode::Script : Mode::Template;
      sc.fuel = 60000;
      Compiled probe(sc.source, sc.mode, "probe.tang");
      if (!probe.ok()) {
        continue;
      }
      Outcome a = run(sc, 0);
      Outcome b = run(sc, 1);
      ASSERT_TRUE(a.created && b.created) << seed;
      EXPECT_EQ(a.key(), b.key()) << "seed " << seed << ":\n" << sc.source;
      entries += b.stats.entries;
      ++compared;
    }
  }
  EXPECT_GT(compared, 100u);
  EXPECT_GT(entries, 100u) << "compiled code ran: the comparison is not vacuous";
}

TEST(Jit, AnExecutionThatTakesTheEnvironmentsThresholdGivesTheInterpretersRun) {
  // GLTANG_TEST_JIT_THRESHOLD=1 over the whole suite is the third differential;
  // this is its smallest instance: a harness-made execution with the default
  // (-1) takes the environment's choice, and either way is the same run.
  Scenario sc;
  sc.source = kLoop;
  Outcome env = run(sc, -1);
  Outcome off = run(sc, 0);
  EXPECT_EQ(env.key(), off.key());
}

#else  // GLTANG_WITH_JIT

// ---------------------------------------------------------------------------
// The interpreter-only build (JIT=no)
// ---------------------------------------------------------------------------

TEST(NoJit, TheHostApiReportsTheJitAsAbsentAndTheRunIsTheInterpreters) {
  EXPECT_FALSE(gltang_jit_built());
  Scenario sc;
  sc.source = kLoop;
  Compiled page(sc.source, sc.mode, "nojit.tang");
  ASSERT_TRUE(page.ok());
  Config config;
  Context context(page.program, config);
  ASSERT_TRUE(context.ok());
  EXPECT_EQ(gltang_execution_set_jit_threshold(context.execution, 1), GLTANG_ERR_UNSUPPORTED);
  EXPECT_EQ(gltang_execution_set_jit_threshold(context.execution, 0), GLTANG_ERR_UNSUPPORTED);
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(context.raw(), "454950499500");
  GLTANG_JitStats stats;
  std::memset(&stats, 0xAB, sizeof stats);
  EXPECT_EQ(gltang_execution_jit_stats(context.execution, &stats), GLTANG_OK);
  EXPECT_EQ(stats.functions_compiled + stats.compile_failures + stats.functions_discarded + stats.entries + stats.returns + stats.deopts +
                stats.refused_pauses + stats.refused_unwinds + stats.slow_polls,
      0u);
  EXPECT_EQ(gltang_execution_jit_stats(nullptr, &stats), GLTANG_ERR_INVALID);
}

#endif  // GLTANG_WITH_JIT

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
