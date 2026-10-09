// Natives called from compiled code (spec-runtime-calls, story 9; CAP-7, AD-28).
//
// A library native is entered by one function, the shared wrapper, from the
// interpreter's CALL and from compiled code's thunks alike; it opens the native's
// activation record, runs the native and closes the record. Compiled code calls a
// library native, and loads a library member, with no exit. Every test here is a
// differential like test_jit_calls.cpp's: one scenario with the JIT off (threshold
// 0) and on (threshold 1), compared in everything observable, plus the statistics
// that say the run was not vacuous. Each test has a watchdog: a hang is a failure.

#include "exec_harness.h"
#include "fuzz/gen.h"
#include "jit_harness.h"
#include "test_helpers.h"
#include "watchdog.h"

#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <ghoti.io/runtime-core/runtime-core.h>

using namespace jt;

namespace {

/// The library every scenario runs against: two host natives of the public API and the engine's test natives.
void add_natives(Context & context) {
  context.add_native_library();
}

#ifdef GLTANG_WITH_JIT

const char * kSourceMax = "576460752303423487";  // the largest small integer

Scenario native_scenario(const std::string & source) {
  Scenario sc;
  sc.source = source;
  sc.script = true;
  sc.before = add_natives;
  return sc;
}

/// What a clean library run looks like in the statistics: the compiled calls were made, nothing was refused.
void expect_clean_natives(const Outcome & jit) {
  EXPECT_EQ(jit.stats.hook_argument_errors, 0u);
  EXPECT_EQ(jit.stats.rebuild_failures, 0u);
  EXPECT_EQ(jit.stats.compile_failures, 0u);
  EXPECT_EQ(jit.native_depth, 0u) << "every native record and JIT record was left";
}


std::string loop_source(int n) {
  return "function run(n) { use inc; s = 0; i = 0; while (i < n) { s = inc(s); i = i + 1; } return s; }\n"
         "print(run(" + std::to_string(n) + ")); print(run(" + std::to_string(n) + "));\n";
}

TEST(NativeCalls, ALoopOfLibraryCallsInACompiledFunctionMakesNoExit) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Outcome plain, jit, few;
  expect_same(native_scenario(loop_source(1000)), &plain, &jit);
  expect_same(native_scenario(loop_source(10)), nullptr, &few);
  EXPECT_EQ(jit.raw, "10001000");
  expect_clean_natives(jit);
  EXPECT_EQ(plain.stats.native_calls, 0u);
  EXPECT_EQ(jit.stats.native_calls, 2000u) << "every call of the loop is a compiled native call";
  EXPECT_EQ(few.stats.native_calls, 20u);
  EXPECT_EQ(jit.stats.member_loads, 2u) << "the `use` is compiled too";
  EXPECT_EQ(jit.stats.native_call_exits_guard + jit.stats.native_exits_stack, 0u);
  EXPECT_EQ(jit.stats.deopts, few.stats.deopts) << "the exits do not grow with the loop: none of them is at a library call";
}


// ---------------------------------------------------------------------------
// The record the wrapper opens, and the depth it costs
// ---------------------------------------------------------------------------

TEST(NativeCalls, ANativeRunsInsideARecordOfItsOwnAndTheNativeDepthItSeesIsTheSameInBothTiers) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc = native_scenario(
      "function probe() { use depth; use records; return depth() * 100 + records(); }\n"
      "print(probe()); print(probe());\n");
  Outcome plain = run(sc, 0);
  Outcome jit = run(sc, 1);
  // One unit of the native-depth budget and one record in the interpreter. Under
  // compiled code the run's JIT record is a second record, whose unit the wrapper
  // hands back for as long as the native runs: the budget sees one unit in both.
  EXPECT_EQ(plain.raw, "101101");
  EXPECT_EQ(jit.raw, "102102");
  EXPECT_EQ(plain.native_depth, 0u);
  EXPECT_EQ(jit.native_depth, 0u);
  EXPECT_GE(jit.stats.native_calls, 2u);
}

TEST(NativeCalls, ABudgetOfNativeDepthRefusesTheSameCallInBothTiers) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // The budget swept from none up: the native's record is refused by a full budget
  // and the call's value is the recursion-limit error; under compiled code the JIT
  // record's unit is handed back, so a budget the interpreter's call fits in is a
  // budget the compiled call fits in, and a run whose compiled entry is refused
  // finishes in the interpreter with the interpreted output.
  for (uint64_t budget = 0; budget <= 4; ++budget) {
    Scenario sc = native_scenario(
        "function f(n) { use inc; return inc(n); }\n"
        "print(f(1) as string); print(f(2) as string); print(f(3) as string);\n");
    sc.native_depth = budget;
    Outcome plain, jit;
    expect_same(sc, &plain, &jit);
    EXPECT_EQ(jit.native_depth, 0u) << "budget " << budget;
    if (budget == 0) {
      EXPECT_NE(plain.raw, "234") << "a full budget refuses the call: the value is an error, not the sum";
    }
    else {
      EXPECT_EQ(plain.raw, "234") << "budget " << budget;
    }
    if (budget >= 2) {
      EXPECT_GE(jit.stats.native_calls, 3u) << "budget " << budget << ": the record of the run is entered and the native is called from compiled code";
    }
    if (HasFailure()) {
      return;
    }
  }
}

// ---------------------------------------------------------------------------
// Arguments
// ---------------------------------------------------------------------------

std::string args_of(int n, const char * base) {
  std::string s;
  for (int i = 0; i < n; ++i) {
    s += (i ? ", " : "") + std::string(base) + " + " + std::to_string(i);
  }
  return s;
}

TEST(NativeCalls, ANativeOfEveryArityFromZeroToSixteenIsCalledFromCompiledCodeUpToFifteenAndIsAnExitAtSixteen) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // runtime-jit's native call takes the callee and fifteen arguments at most (sixteen
  // words): a call of sixteen arguments is an exit, and gives the interpreter's value.
  for (int n = 0; n <= 16; ++n) {
    Scenario sc = native_scenario(
        "function run() { use sum; s = 0; i = 0; while (i < 20) { s = s + sum(" + args_of(n, "i") + "); i = i + 1; } return s; }\n"
        "print(run()); print(run());\n");
    Outcome plain, jit;
    expect_same(sc, &plain, &jit);
    if (n <= 15) {
      expect_clean_natives(jit);
    }
    EXPECT_EQ(jit.stats.native_calls, n <= 15 ? 40u : 0u) << n << " arguments";
    EXPECT_EQ(jit.stats.native_call_exits_guard + jit.stats.native_exits_stack, 0u) << n << " arguments";
    if (HasFailure()) {
      ADD_FAILURE() << "with " << n << " arguments";
      return;
    }
  }
}

TEST(NativeCalls, ReferencesPassedToANativeInPaddedFramesAreIntactUnderTortureAndTheNativeAllocates) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // Callees of 5, 6, 7, 8, 12, 13, 14 and 15 parameters hold arrays, hand the last
  // to a native that allocates (so it collects, with every compiled frame below it),
  // and read every array afterwards.
  for (int n : {1, 5, 6, 7, 8, 12, 13, 14, 15}) {
    std::string formals, actuals;
    for (int i = 0; i < n; ++i) {
      formals += (i ? ", " : "") + std::string("p") + std::to_string(i);
      actuals += (i ? ", " : "") + std::string("[i + ") + std::to_string(i) + "]";
    }
    std::string firsts;
    for (int i = 0; i < n; ++i) {
      firsts += std::string(i ? " + " : "") + "first(p" + std::to_string(i) + ")";
    }
    Scenario sc = native_scenario(
        "function first(x) { return x[0]; }\n"
        "function g(" + formals + ") { use alloc_ref; r = alloc_ref(p" + std::to_string(n - 1) + "); return " + firsts + " + first(first(r)); }\n"
        "function run() { s = 0; i = 0; while (i < 12) { s = s + g(" + actuals + "); i = i + 1; } return s; }\n"
        "print(run()); print(run());\n");
    sc.torture = 1;
    sc.verify = 1;
    sc.moving = 1;
    sc.gc_at_native = true;
    Outcome plain, jit;
    expect_same(sc, &plain, &jit);
    expect_clean_natives(jit);
    EXPECT_GE(jit.stats.native_calls, 24u) << n << " parameters";
    if (HasFailure()) {
      ADD_FAILURE() << "with " << n << " parameters";
      return;
    }
  }
}

std::string deep_alloc_source(int depth) {
  return "function deep(n, a, b) {\n"
         "  use alloc_ref;\n"
         "  if (n == 0) { r = alloc_ref(a); i = 0; while (i < 3) { i = i + 1; } return r[0]; }\n"
         "  r = deep(n - 1, b, a);\n"
         "  return r;\n"
         "}\n"
         "a = [1]; b = [2]; x = deep(" + std::to_string(depth) + ", a, b); print(x[0]); print(a[0]); print(b[0]);\n";
}

TEST(NativeCalls, AChainOfSixtyCompiledFramesHoldingReferencesSurvivesANativeThatAllocatesAtItsBottomUnderTorture) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc = native_scenario(deep_alloc_source(60));
  sc.torture = 1;
  sc.verify = 1;
  sc.moving = 1;
  sc.gc_at_native = true;
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  EXPECT_EQ(jit.raw, "112");
  EXPECT_GE(jit.stats.deepest_chain, 60u);
  EXPECT_GT(jit.stack_moves, 0u) << "the guest stack started small and grew under the chain: a capacity probe that did not probe nothing";
  EXPECT_GE(jit.stats.native_calls, 1u);
  EXPECT_GE(jit.stats.member_loads, 61u) << "every level `use`s the native: each load allocates";
  expect_clean_natives(jit);
}

// ---------------------------------------------------------------------------
// The callee-value guard, statuses, exits
// ---------------------------------------------------------------------------

TEST(NativeCalls, AMemberReassignedToAnotherNativeFailsTheGuardAndTheInterpreterCallsTheNewValue) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc = native_scenario(
      "function run() { use inc; use sum; s = 0; i = 0; while (i < 10) { s = inc(s); if (i == 4) { inc = sum; } i = i + 1; } return s; }\n"
      "print(run());\n");
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  EXPECT_EQ(jit.raw, "5");
  EXPECT_GE(jit.stats.native_call_exits_guard, 1u);
  EXPECT_LE(jit.stats.native_calls, 5u);
}

TEST(NativeCalls, ANativeThatAsksCompiledCodeToLeaveIsAnExitAfterTheCallWithItsValueInPlace) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc = native_scenario(
      "function run() { use deopt; s = 0; i = 0; while (i < 10) { s = s + deopt(i); i = i + 1; } return s; }\n"
      "print(run()); print(run()); print(run());\n");
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  EXPECT_EQ(jit.raw, "454545");
  EXPECT_EQ(jit.stats.native_status_deopts, 3u) << "one per activation: the interpreter finishes it";
  EXPECT_EQ(jit.stats.native_calls, 3u) << "the native ran once for each, not again by the interpreter";
  EXPECT_EQ(jit.stats.native_status_unwinds, 0u);
  EXPECT_EQ(jit.stats.last_exit_cause, (uint64_t{1} << 32) | 1u);
  expect_clean_natives(jit);
}

bool big_native(GLTANG_NativeCall * call, void *) {
  static const std::string text(1 << 20, 'x');
  gltang_call_return_string(call, text.data(), text.size(), GLTANG_UNICODE_STRING_TYPE_TRUSTED);
  return true;
}

TEST(NativeCalls, ANativeWhoseResultUnwindsTheRunIsAnUnwindStatusAndTheRunEndsAsTheInterpretersDoes) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc = native_scenario(
      "function run() { use big; s = 0; i = 0; while (i < 100) { s = s + big().length; i = i + 1; } return s; }\n"
      "print(run());\n");
  sc.fuel = 20000;
  sc.before = [](Context & context) {
    add_natives(context);
    ASSERT_EQ(gltang_library_add_native(context.library(), "big", big_native, nullptr), GLTANG_OK);
  };
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  EXPECT_FALSE(plain.finished) << "the budget ran out inside the native's answer";
  EXPECT_GE(jit.stats.native_status_unwinds, 1u) << "the unwind arrived as a status, and was not read as a value";
  EXPECT_EQ(jit.stats.last_exit_cause & (uint64_t{1} << 32), uint64_t{1} << 32);
  expect_clean_natives(jit);
}

TEST(NativeCalls, ABackendThatRefusesNativesLeavesTheSitesAsExitsAndSaysSo) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // The `use` is a global, so the function's first operation is not an exit and the
  // function is compiled: its call site is the one natives being unavailable declines.
  Scenario sc = native_scenario(
      "use inc;\n"
      "function run(n) { global inc; s = 0; i = 0; while (i < n) { s = inc(s); i = i + 1; } return s; }\n"
      "print(run(200)); print(run(200));\n");
  sc.natives_off = true;
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  EXPECT_EQ(jit.raw, "200200");
  EXPECT_EQ(jit.stats.native_calls, 0u);
  EXPECT_GE(jit.stats.native_sites_unsupported, 1u) << "the call site is counted, not guessed";
  EXPECT_GT(jit.stats.deopts, 0u) << "the call is an exit";
  Scenario control = sc;
  control.natives_off = false;
  Outcome with;
  expect_same(control, nullptr, &with);
  EXPECT_EQ(with.stats.native_sites_unsupported, 0u);
  EXPECT_EQ(with.stats.native_calls, 400u) << "the same program with natives available makes every call from compiled code";
}

TEST(NativeCalls, ANativeStackBudgetSweptAcrossTheNeedOfACallIsAnExitBeforeItAndNeverAFault) {
  GLTANG_REQUIRE_JIT_BACKEND();
  uint64_t exits_seen = 0;
  uint64_t calls_seen = 0;
  uint64_t mixed = 0;
  for (uint64_t bytes = 1024; bytes <= 96 * 1024; bytes += 1024) {
    Scenario sc = native_scenario(loop_source(30));
    sc.native_stack_bytes = bytes;
    Outcome plain, jit;
    expect_same(sc, &plain, &jit);
    EXPECT_EQ(jit.raw, "3030") << bytes << " bytes";
    exits_seen += jit.stats.native_exits_stack;
    calls_seen += jit.stats.native_calls;
    mixed += jit.stats.native_exits_stack != 0 && jit.stats.native_calls != 0;
    if (HasFailure()) {
      ADD_FAILURE() << "with a budget of " << bytes << " bytes";
      return;
    }
  }
  EXPECT_GT(exits_seen, 0u) << "the sweep started below the need of a call";
  EXPECT_GT(calls_seen, 0u) << "and ended above it";
}

// ---------------------------------------------------------------------------
// A native that re-enters guest code, and one that is resumable
// ---------------------------------------------------------------------------

TEST(NativeCalls, ANativeThatCallsAGuestFunctionRunsItAsANestedActivationInBothTiers) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc = native_scenario(
      "function leaf(x, y) { return x * 2 + y; }\n"
      "function run() { use reenter; s = 0; i = 0; while (i < 10) { s = s + reenter(leaf, i, 1); i = i + 1; } return s; }\n"
      "print(run()); print(run());\n");
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  EXPECT_EQ(jit.raw, "100100");
  EXPECT_EQ(jit.stats.native_calls, 20u);
  EXPECT_GE(jit.stats.entries, 21u) << "the nested activation is compiled too";
  expect_clean_natives(jit);
}

std::string reenter_chain_source(int depth) {
  // The leaf allocates (a native that does), so the nested activation collects with
  // every compiled frame of the chain below it; the chain holds two arrays, swapped
  // at each level, which the top reads afterwards.
  return "function leaf(x) { use alloc_n; r = alloc_n(3); k = 0; while (k < 3) { k = k + 1; } return x + k; }\n"
         "function deep(n, a, b) {\n"
         "  use reenter;\n"
         "  if (n == 0) { t = reenter(leaf, 1) + reenter(leaf, 2); return a; }\n"
         "  r = deep(n - 1, b, a);\n"
         "  return r;\n"
         "}\n"
         "a = [1]; b = [2]; x = deep(" + std::to_string(depth) + ", a, b); print(x[0]); print(a[0]); print(b[0]);\n";
}

TEST(NativeCalls, EveryCompiledFrameBelowANestedActivationSeesItsReferencesUpdatedAcrossCollectionsInsideIt) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc = native_scenario(reenter_chain_source(40));
  sc.torture = 1;
  sc.verify = 1;
  sc.moving = 1;
  sc.gc_at_native = true;
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  EXPECT_EQ(jit.raw, "112");
  EXPECT_GT(jit.stack_moves, 0u) << "the guest stack grew under the chain";
  EXPECT_GE(jit.stats.deepest_chain, 40u);
  EXPECT_GE(jit.stats.native_calls, 2u + 2u * 1u);
  expect_clean_natives(jit);
}

TEST(NativeCalls, ABudgetOfFuelThatRunsOutInsideANestedActivationUnwindsToItsBoundaryAsTheInterpreterDoes) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // Every budget from 1 up is a pause or an unwind at a different place: in the
  // caller, in the nested function, at its entry, and in the call. A pause inside the
  // nested activation is an unwind of that call (the record is nested, AD-5); the
  // run ends, and ends the same way in both tiers.
  uint64_t unwound_inside = 0;
  for (uint64_t budget = 1; budget <= 400; budget += 3) {
    Scenario sc = native_scenario(
        "function leaf(x) { k = 0; while (k < 4) { k = k + 1; } return x + k; }\n"
        "function run() { use reenter; s = 0; i = 0; while (i < 6) { s = s + reenter(leaf, i); i = i + 1; } return s; }\n"
        "print(run());\n");
    sc.fuel = budget;
    Outcome plain, jit;
    expect_same(sc, &plain, &jit);
    unwound_inside += !plain.finished;
    if (HasFailure()) {
      ADD_FAILURE() << "with a fuel budget of " << budget;
      return;
    }
  }
  EXPECT_GT(unwound_inside, 10u) << "most budgets end the run";
}

TEST(NativeCalls, ANativeDepthBudgetSweptFromOneUpRefusesTheSameReentryAtTheSamePollAndFuelInBothTiers) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // Each level of the recursion is a native (one record) that opens a re-entry (a
  // second); compiled, each level is also a JIT record the budget would count. The
  // wrapper hands those units back, so the budget refuses at the same level, at
  // the same poll, with the same fuel total, whichever tier runs it.
  uint64_t refused = 0;
  uint64_t completed = 0;
  uint64_t refused_compiled = 0;
  for (uint64_t budget = 1; budget <= 24; ++budget) {
    Scenario sc = native_scenario(
        "function r(n) { use reenter; if (n == 0) { return 0; } return reenter(r, n - 1) + 1; }\n"
        "print(r(8) as string);\n");
    sc.native_depth = budget;
    Outcome plain, jit;
    expect_same(sc, &plain, &jit);
    EXPECT_EQ(plain.fuel, jit.fuel) << "budget " << budget;
    EXPECT_EQ(jit.native_depth, 0u) << "budget " << budget;
    refused += plain.raw != "8";
    completed += plain.raw == "8";
    refused_compiled += jit.stats.native_enters_refused;
    if (HasFailure()) {
      ADD_FAILURE() << "with a native-depth budget of " << budget;
      return;
    }
  }
  EXPECT_GT(refused, 3u);
  EXPECT_GT(completed, 3u);
  // A compiled call whose run was entered is never refused its record: the entry was allowed
  // because the interpreter-equivalent depth had room, and the wrapper hands the JIT records' units
  // back before it asks for its own. (A refused record is counted when memory, not depth, refuses.)
  EXPECT_EQ(refused_compiled, 0u);
}

TEST(NativeCalls, AResumableNativeIsReachedOnlyThroughAnExitAndTheInterpreterRunsAndResumesIt) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc = native_scenario(
      "function g() { k = 0; while (k < 5) { k = k + 1; } return k + 2; }\n"
      "function run() { use resumable; return resumable(g); }\n"
      "print(run()); print(run());\n");
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  EXPECT_EQ(jit.raw, "2121");
  EXPECT_EQ(jit.stats.native_calls, 0u) << "compiled code never enters a resumable native";
  EXPECT_GE(jit.stats.deopts, 2u) << "the call is an exit";
}

TEST(NativeCalls, AResumableNativePausedInsideTheGuestCodeItAsksForResumesToTheUninterruptedOutput) {
  GLTANG_REQUIRE_JIT_BACKEND();
  for (uint64_t step : {7u, 13u, 40u}) {
    Scenario sc = native_scenario(
        "function g() { k = 0; while (k < 5) { k = k + 1; } return k + 2; }\n"
        "function run() { use resumable; return resumable(g); }\n"
        "print(run()); print(run());\n");
    sc.fuel = step;
    sc.step = step;
    Outcome plain, jit;
    expect_same(sc, &plain, &jit);
    EXPECT_EQ(jit.raw, "2121") << "resumed after every " << step << " units of fuel";
    EXPECT_GE(jit.pauses.size(), 3u);
    EXPECT_EQ(jit.stats.native_calls, 0u);
    if (HasFailure()) {
      return;
    }
  }
}


// ---------------------------------------------------------------------------
// Generated call graphs that call the library
// ---------------------------------------------------------------------------

TEST(NativeCalls, GeneratedCallGraphsThatCallTheLibraryAreTheSameCompiledAndInterpreted) {
  GLTANG_REQUIRE_JIT_BACKEND();
  const char * seeds_text = std::getenv("GLTANG_CALL_GRAPH_SEEDS");
  const char * first_text = std::getenv("GLTANG_CALL_GRAPH_FIRST");
  const uint64_t kSeeds = seeds_text && *seeds_text ? std::strtoull(seeds_text, nullptr, 10) : (tt::heavy_instruments() ? 50 : 300);
  const uint64_t kFirst = first_text && *first_text ? std::strtoull(first_text, nullptr, 10) : 1;
  uint64_t native_calls = 0, loads = 0, with_calls = 0, status_exits = 0, guard_exits = 0;
  for (uint64_t seed = kFirst; seed < kFirst + kSeeds; ++seed) {
    Scenario sc = native_scenario(gen::generate_calls(seed, true).source);
    sc.fuel = 20000000;
    sc.calls = 512;
    Outcome plain, jit;
    expect_same(sc, &plain, &jit);
    ASSERT_FALSE(HasFailure()) << "seed " << seed << ":\n" << sc.source;
    native_calls += jit.stats.native_calls;
    loads += jit.stats.member_loads;
    with_calls += jit.stats.native_calls > 0;
    status_exits += jit.stats.native_status_deopts + jit.stats.native_status_unwinds;
    guard_exits += jit.stats.native_call_exits_guard;
    EXPECT_EQ(jit.stats.hook_argument_errors, 0u) << "seed " << seed;
    EXPECT_EQ(jit.stats.rebuild_failures, 0u) << "seed " << seed;
    EXPECT_EQ(jit.native_depth, 0u) << "seed " << seed;
  }
  EXPECT_GT(native_calls, kSeeds * 5) << "the comparison is not vacuous: native calls were compiled";
  EXPECT_GT(with_calls, kSeeds / 3);
  EXPECT_GT(loads, kSeeds);
  EXPECT_GT(status_exits, 0u) << "a native asked compiled code to leave";
  std::printf("  generated library call graphs: %llu programs, %llu compiled native calls, %llu member loads, %llu status exits, %llu guard exits\n",
      (unsigned long long)kSeeds, (unsigned long long)native_calls, (unsigned long long)loads, (unsigned long long)status_exits, (unsigned long long)guard_exits);
}

TEST(NativeCalls, GeneratedLibraryCallGraphsUnderTortureAMovingStackAndANativeCollectionAreTheSame) {
  GLTANG_REQUIRE_JIT_BACKEND();
  const uint64_t kSeeds = tt::heavy_instruments() ? 12 : 40;
  for (uint64_t seed = 1000; seed < 1000 + kSeeds; ++seed) {
    Scenario sc = native_scenario(gen::generate_calls(seed, true).source);
    sc.fuel = 20000000;
    sc.torture = 1;
    sc.verify = 1;
    sc.moving = 1;
    sc.gc_at_native = true;
    Outcome plain, jit;
    expect_same(sc, &plain, &jit);
    ASSERT_FALSE(HasFailure()) << "seed " << seed << ":\n" << sc.source;
  }
}

// ---------------------------------------------------------------------------
// Retired code with a nested record open
// ---------------------------------------------------------------------------

TEST(NativeCalls, ARetiredCodeListStaysWithinTheDiscardedFunctionsWhenTheDiscardsAreMadeInsideANestedActivation) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // The thrashing program of test_jit_calls.cpp, run as a nested activation: the top
  // level calls the driver of each group through a native that re-enters guest code,
  // so every function is discarded (at the eighth failed guard) while the top level's
  // own JIT record is open, and core cannot free the code it retires until that record
  // is left. The claim to test is that the retired list is bounded by the functions of
  // the program (each is discarded at most once and never compiled again): its peak
  // is read from the run and must not pass twice the number discarded.
  const char * n = std::getenv("GLTANG_LONG_RUN_FUNCTIONS");
  const int kFunctions = n && *n && std::atoi(n) > 0 ? std::atoi(n) : (tt::heavy_instruments() ? 12 : 40);
  std::string source = "M = " + std::string(kSourceMax) + ";\nuse reenter;\n";
  for (int i = 0; i < kFunctions; ++i) {
    source += "function f" + std::to_string(i) + "(x) { return x + x; }\n";
  }
  const int kGroup = 40;
  const int kGroups = (kFunctions + kGroup - 1) / kGroup;
  for (int g = 0; g < kGroups; ++g) {
    source += "function drive" + std::to_string(g) + "(i) { s = 0;\n";
    for (int i = g * kGroup; i < kFunctions && i < (g + 1) * kGroup; ++i) {
      source += "  s = s + f" + std::to_string(i) + "(i);\n";
    }
    source += "  return s; }\n";
  }
  // `outer` is compiled at its first poll and calls the natives from compiled code, so its JIT
  // record is open for the whole of the loop: the nested drivers are discarded under it.
  source += "function outer(M) { global reenter; ";
  for (int g = 0; g < kGroups; ++g) {
    source += "global drive" + std::to_string(g) + "; ";
  }
  source += "k = 0; while (k < 12) {\n";
  for (int g = 0; g < kGroups; ++g) {
    source += "  t = reenter(drive" + std::to_string(g) + ", M);\n";
  }
  source += "  k = k + 1; }\n return 0; }\n";
  source += "outer(M); print(reenter(drive0, 1));\n";
  Scenario sc = native_scenario(source);
  sc.fuel = 2000000000;
  Outcome plain = run(sc, 0);
  Outcome jit = run(sc, 1);
  ASSERT_TRUE(jit.finished) << "the thrashing program runs to its end";
  EXPECT_EQ(plain.key(), jit.key());
  EXPECT_GE(jit.stats.functions_discarded, static_cast<uint64_t>(kFunctions) / 2);
  EXPECT_GT(jit.retired_peak, 0u) << "the discards were made under an open record, so the retired list did hold code: a measure of zero here would measure nothing";
  EXPECT_LE(jit.retired_peak, 2u * jit.stats.functions_discarded)
      << "the retired list stays within twice the discarded functions (peak " << jit.retired_peak << " for " << jit.stats.functions_discarded << " discarded)";
  EXPECT_EQ(jit.stats.native_status_unwinds + jit.stats.rebuild_failures, 0u);
  std::printf("  thrashing program in nested activations: %d functions, %llu discarded, retired peak %llu, %llu ranges registered at the end, %llu native calls, %llu entries\n", kFunctions,
      (unsigned long long)jit.stats.functions_discarded, (unsigned long long)jit.retired_peak, (unsigned long long)jit.registered, (unsigned long long)jit.stats.native_calls,
      (unsigned long long)jit.stats.entries);
}

TEST(NativeCalls, TheExitsAtLibraryCallSitesCountTowardTheDiscardLimitOnlyForTheGuard) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // A callee-value guard that fails eight times discards the function; a native
  // that asks compiled code to leave (a status) never does, however many times it
  // happens.
  Scenario guard = native_scenario(
      "function run(f) { use inc; use sum; if (f) { inc = sum; } return inc(3); }\n"
      "t = 0; for (i = 0; i < 12; i += 1) { t = t + run(true) + run(false); } print(t);\n");
  Outcome plain, jit;
  expect_same(guard, &plain, &jit);
  EXPECT_GE(jit.stats.native_call_exits_guard, 8u);
  EXPECT_GE(jit.stats.functions_discarded, 1u) << "the eighth failed guard discards the function";
  Scenario status = native_scenario(
      "function run(x) { use deopt; return deopt(x) + 1; }\n"
      "t = 0; for (i = 0; i < 40; i += 1) { t = t + run(i); } print(t);\n");
  expect_same(status, &plain, &jit);
  EXPECT_EQ(jit.stats.native_status_deopts, 40u);
  EXPECT_EQ(jit.stats.functions_discarded, 0u) << "a status is not an exit of the function";
}

TEST(NativeCalls, ALibraryCallWithTooManyArgumentsOrACalleeNothingNamesStaysAnExitWithTheInterpretersValue) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc = native_scenario(
      "function good(n) { use inc; s = 0; i = 0; while (i < n) { s = inc(s); i = i + 1; } return s; }\n"
      "function wide() { use sum; return sum(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16); }\n"
      "function viaparameter(g, i) { return g(i); }\n"
      "use inc as h; print(good(10)); print(wide()); print(viaparameter(h, 4));\n");
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  EXPECT_EQ(plain.raw, "10" "1496" "5") << "sum of 1..16 weighted by position is 1496";
  EXPECT_EQ(jit.stats.native_calls, 10u) << "only the calls of `good` are compiled: sixteen arguments and a parameter's value are exits";
}


TEST(NativeCalls, AContextPausedBetweenLibraryCallsAndInsideAResumableNativeResumesOnAnotherThreadWithTheSameOutput) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // The compiled code is gone from the native stack at a pause (the guest frames hold
  // everything: the chain was rebuilt, and a resumable native's continuation is a value in
  // the caller's frame), so the context moves to another thread; under TSan the hand-over
  // is checked for a race.
  const char * source =
      "function g() { k = 0; while (k < 6) { k = k + 1; } return k; }\n"
      "function run(n) { use inc; use sum; use resumable; s = 0; i = 0; while (i < n) { s = sum(inc(s), i, 1); i = i + 1; } return s + resumable(g); }\n"
      "t = 0; for (m = 0; m < 12; m += 1) { t = t + run(30) % 11; print(t); }\n";
  Compiled compiled(source, Mode::Script, "hop.tang");
  ASSERT_TRUE(compiled.ok());
  Config reference_config;
  reference_config.jit_threshold = 0;
  Context reference(compiled.program, reference_config);
  ASSERT_TRUE(reference.ok());
  reference.add_native_library();
  ASSERT_TRUE(reference.execute());
  Config config;
  config.fuel = 300;
  config.jit_threshold = 1;
  Context context(compiled.program, config);
  ASSERT_TRUE(context.ok());
  context.add_native_library();
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
    ASSERT_LT(++hops, 2000);
  }
  EXPECT_GT(hops, 5);
  EXPECT_EQ(context.raw(), reference.raw());
  EXPECT_EQ(context.describe(), reference.describe());
  EXPECT_GT(context.jit_stats().native_calls, 100u) << "library calls were made from compiled code on the way";
  EXPECT_GT(context.jit_stats().refused_pauses, 2u) << "pauses were taken inside compiled code, between library calls";
  EXPECT_EQ(context.jit_stats().functions_discarded, 0u);
}

TEST(NativeCalls, AMemberLoadThatYieldsNothingCompiledStaysAnExit) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc = native_scenario(
      "function run() { use math; use random; use nothing_provides_this; s = 0; i = 0; while (i < 5) { s = s + inc2(i); i = i + 1; } return s; }\n"
      "function inc2(x) { return x + 1; }\n"
      "print(run() as string);\n");
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
}


#else  // GLTANG_WITH_JIT

TEST(NativeCallsNoJit, ANativeRunsInsideARecordOfItsOwnAndAFullBudgetRefusesIt) {
  Compiled page("use depth; use records; use inc; print(depth() * 100 + records()); print(inc(1) as string);", Mode::Script, "nojit.tang");
  ASSERT_TRUE(page.ok());
  for (uint64_t budget : {GRCORE_UNLIMITED, uint64_t{1}, uint64_t{0}}) {
    Config config;
    config.native_depth = budget;
    Context context(page.program, config);
    ASSERT_TRUE(context.ok());
    add_natives(context);
    ASSERT_TRUE(context.execute());
    EXPECT_EQ(grcore_context_depth(context.context, GRCORE_DEPTH_NATIVE), 0u) << "every record was left";
    if (budget == 0) {
      EXPECT_EQ(context.raw().find("101"), std::string::npos) << "the refused call is an error, and the probe never ran: " << context.raw();
    }
    else {
      EXPECT_EQ(context.raw(), "1012") << "one unit of native depth, one record";
    }
  }
}

TEST(NativeCallsNoJit, ANativeThatReentersGuestCodeAndAResumableOneRunInTheInterpreter) {
  Compiled page(
      "function leaf(x) { return x * 2; }\n"
      "function g() { return 5; }\n"
      "use reenter; use resumable; print(reenter(leaf, 21)); print(resumable(g));",
      Mode::Script, "nojit.tang");
  ASSERT_TRUE(page.ok());
  Config config;
  Context context(page.program, config);
  ASSERT_TRUE(context.ok());
  add_natives(context);
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(context.raw(), "4215");
  GLTANG_JitStats stats = context.jit_stats();
  EXPECT_EQ(stats.native_calls + stats.member_loads + stats.native_call_exits_guard + stats.native_exits_stack + stats.native_status_deopts +
                stats.native_status_unwinds + stats.native_sites_unsupported + stats.native_enters_refused,
      0u);
}

#endif  // GLTANG_WITH_JIT

}  // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  tt::install_watchdog(240);
  return RUN_ALL_TESTS();
}
