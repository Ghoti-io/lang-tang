// Calls between compiled functions (spec-runtime-calls, story 8; AD-28).
//
// A call of a declared guest function from compiled code is a call through the
// callee's entry slot, behind a guard on the callee value, with lang-tang's own
// push, pop, compile-at-call and deopt hooks over runtime-core. Every test here
// is a differential like test_jit.cpp's: one scenario with the JIT off (threshold
// 0) and on (threshold 1: every function tiers up at its first poll), compared in
// everything observable (output, rendered output, result, the error list with its
// template chain, fuel, the polls and their identities, every pause), plus the
// statistics that say the run was not vacuous: compiled calls were made, no call
// between two compiled functions left compiled code, and no hook was handed an
// argument it refuses. Each test has a watchdog: a hang is a failure.

#include "exec_harness.h"
#include "fuzz/gen.h"
#include "jit_harness.h"
#include "test_helpers.h"
#include "watchdog.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <ghoti.io/runtime-core/runtime-core.h>

using namespace jt;

#ifdef GLTANG_WITH_JIT

namespace {

const char * kSourceMax = "576460752303423487";  // the largest small integer

std::string join(const std::vector<std::string> & parts, const char * with = ", ") {
  std::string s;
  for (size_t i = 0; i < parts.size(); ++i) {
    s += (i ? with : "") + parts[i];
  }
  return s;
}

std::string params(int n, const char * prefix = "a") {
  std::vector<std::string> p;
  for (int i = 0; i < n; ++i) {
    p.push_back(prefix + std::to_string(i));
  }
  return join(p);
}


uint64_t exits(const GLTANG_JitStats & s) {
  return s.call_exits_remembered + s.call_exits_push_refused + s.call_exits_callee_guard + s.call_exits_native_stack;
}

/// What a clean call-heavy run looks like in the statistics.
void expect_clean_calls(const Outcome & jit, uint64_t at_least) {
  EXPECT_GE(jit.stats.calls, at_least) << "compiled calls were made";
  EXPECT_EQ(exits(jit.stats), 0u) << "no call between compiled functions left compiled code";
  EXPECT_EQ(jit.stats.hook_argument_errors, 0u);
  EXPECT_EQ(jit.stats.rebuild_failures, 0u);
  EXPECT_EQ(jit.stats.compile_failures, 0u);
}

// ---------------------------------------------------------------------------
// Direct calls, every shape
// ---------------------------------------------------------------------------

/// `f<n>` takes n small integers and sums them (7 for none); `run` calls it in a loop.
std::string sum_function(int n, const std::string & name) {
  std::string body = n == 0 ? "7" : params(n, "a");
  std::vector<std::string> sum;
  for (int i = 0; i < n; ++i) {
    sum.push_back("a" + std::to_string(i));
  }
  return "function " + name + "(" + params(n) + ") { return " + (n == 0 ? std::string("7") : join(sum, " + ")) + "; }\n";
}

std::string call_args(int n, const char * with) {
  std::vector<std::string> args;
  for (int i = 0; i < n; ++i) {
    args.push_back(std::string(with) + (i ? " + " + std::to_string(i) : ""));
  }
  return join(args);
}

TEST(JitCalls, ADeclaredGlobalFunctionOfEveryParameterCountIsCalledFromCompiledCode) {
  GLTANG_REQUIRE_JIT_BACKEND();
  for (int n = 0; n <= 15; ++n) {
    Scenario sc;
    sc.source = sum_function(n, "f") +
        "function run() { s = 0; i = 0; while (i < 20) { s = s + f(" + call_args(n, "i") + "); i = i + 1; } return s; }\n"
        "print(run()); print(run());\n";
    sc.script = true;
    Outcome plain, jit;
    expect_same(sc, &plain, &jit);
    expect_clean_calls(jit, 40);
    EXPECT_EQ(plain.stats.calls, 0u);
    EXPECT_GE(jit.stats.compile_at_call, 1u) << n << " parameters: the callee is compiled at its first call from compiled code";
    if (HasFailure()) {
      ADD_FAILURE() << "with " << n << " parameters";
      return;
    }
  }
}

TEST(JitCalls, ADeclaredLocalFunctionOfEveryParameterCountIsCalledFromCompiledCode) {
  GLTANG_REQUIRE_JIT_BACKEND();
  for (int n = 0; n <= 15; ++n) {
    Scenario sc;
    sc.source =
        "function run() {\n" + sum_function(n, "f") +
        "  s = 0; i = 0; while (i < 20) { s = s + f(" + call_args(n, "i") + "); i = i + 1; } return s; }\n"
        "print(run()); print(run());\n";
    sc.script = true;
    Outcome plain, jit;
    expect_same(sc, &plain, &jit);
    expect_clean_calls(jit, 40);
    if (HasFailure()) {
      ADD_FAILURE() << "with " << n << " parameters";
      return;
    }
  }
}

TEST(JitCalls, RecursionCompilesAndTwoFunctionsThatCallEachOtherThroughANestedDeclarationDoToo) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // Tang resolves a name when it reads it, so a function cannot name one that is
  // declared after it: recursion is a function naming itself, and a call tree is
  // a function naming those declared before it (or, inside a function, the ones
  // it declares itself). Both compile to calls through slots.
  Scenario sc;
  sc.source =
      "function fib(n) { if (n < 2) { return n; } return fib(n - 1) + fib(n - 2); }\n"
      "function twice(n) { function half(k) { return k - 1; } function inc(k) { return k + 2; } return half(n) + inc(n) + half(inc(n)); }\n"
      "function tree(n) { if (n == 0) { return 1; } return tree(n - 1) + tree(n - 1) + twice(n); }\n"
      "print(fib(15)); print(twice(9)); print(tree(8));\n";
  sc.script = true;
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  EXPECT_EQ(plain.raw.substr(0, 3), "610");
  expect_clean_calls(jit, 1500);
  EXPECT_GE(jit.stats.deepest_chain, 14u) << "fib(15) is fifteen frames deep: fourteen compiled calls under the interpreter's entry";
}

// ---------------------------------------------------------------------------
// Entry-form parity: one function entered from the interpreter (flag 1) and from
// a compiled call (flag 0) makes exactly one entry poll per activation
// ---------------------------------------------------------------------------

TEST(JitCalls, EveryParameterCountIsEnteredFromTheInterpreterAndFromCompiledCodeWithOnePollEach) {
  GLTANG_REQUIRE_JIT_BACKEND();
  for (int n = 0; n <= 15; ++n) {
    Scenario sc;
    // `f` is entered by the interpreter at the top level (function 0 leaves
    // compiled code at the first `print`), and by compiled code from `run`.
    sc.source = sum_function(n, "f") +
        "function run() { return f(" + call_args(n, "5") + "); }\n"
        "print(f(" + call_args(n, "3") + ")); print(run()); print(f(" + call_args(n, "1") + ")); print(run());\n";
    sc.script = true;  // counts the polls and records the identity of each
    Outcome plain, jit;
    expect_same(sc, &plain, &jit);
    EXPECT_EQ(plain.polls, jit.polls) << n << " parameters: the polls, entry polls included";
    EXPECT_GT(jit.stats.entries, 3u);
    EXPECT_GE(jit.stats.calls, 2u);
    if (HasFailure()) {
      ADD_FAILURE() << "with " << n << " parameters";
      return;
    }
  }
}

/// A function of n reference parameters that returns the k-th, run under torture.
std::string ref_function(int n, int keep) {
  return "function r(" + params(n) + ") { return a" + std::to_string(keep) + "; }\n";
}

TEST(JitCalls, ReferencesInPaddedFramesSurviveACollectionAtEveryPollUnderTorture) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // Callees of 5, 6, 7, 8, 12, 13, 14 and 15 reference parameters: stack
  // arguments, an odd and an even number of them. A poll in the callee collects
  // (and, in the relocation arm, moves), so every argument must be in the call
  // site's stack map and the callee's frame must update its own copies.
  for (int n : {1, 3, 5, 6, 7, 8, 12, 13, 14, 15}) {
    std::vector<std::string> arrays, args;
    for (int i = 0; i < n; ++i) {
      arrays.push_back("p" + std::to_string(i) + " = [" + std::to_string(100 + i) + "];");
      args.push_back("p" + std::to_string(i));
    }
    Scenario sc;
    sc.source = ref_function(n, n - 1) +
        "function spin() { i = 0; while (i < 4) { i = i + 1; } return i; }\n"
        "function mid(" + params(n) + ") { t = spin(); x = r(" + params(n) + "); t = spin(); return x; }\n" +
        join(arrays, " ") + "\nx = mid(" + join(args) + "); print(x[0]); print(p0[0]);\n";
    sc.script = true;
    sc.torture = 1;
    sc.verify = 1;
    sc.moving = 1;
    sc.on_poll = [](Script &, GRCORE_Context * context, uint64_t) {
      EXPECT_EQ(grheap_collect(grheap_heap_get(context)), GRHEAP_OK);
    };
    Outcome plain, jit;
    expect_same(sc, &plain, &jit);
    EXPECT_EQ(jit.raw, std::to_string(100 + n - 1) + "100") << n << " parameters";
    EXPECT_GE(jit.stats.calls, 3u);
    if (HasFailure()) {
      ADD_FAILURE() << "with " << n << " parameters";
      return;
    }
  }
}

// ---------------------------------------------------------------------------
// Tier-up, uncompilable callees, the callee-value guard
// ---------------------------------------------------------------------------

TEST(JitCalls, ACalleeThatIsColdWhenItsCallerIsCompiledIsCompiledAtTheFirstCallAndCalledDirectlyAfter) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc;
  sc.source =
      "function g(x) { return x + 1; }\n"
      "function f(n) { s = 0; i = 0; while (i < n) { s = s + g(i); i = i + 1; } return s; }\n"
      "print(f(10)); print(f(10000));\n";
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  EXPECT_EQ(plain.raw, "55" "50005000");
  expect_clean_calls(jit, 10000);
  EXPECT_EQ(jit.stats.compile_at_call, 1u) << "g, once, at the first call from f";
  EXPECT_EQ(jit.stats.compile_hook_calls, 1u) << "and never asked again: the slot holds g's code";
  // No caller is recompiled: f and g, and the top level.
  EXPECT_LE(jit.stats.functions_compiled, 3u);
}

TEST(JitCalls, ACalleeTheInterpreterTiersUpIsCalledDirectlyFromThenOnWithoutRecompilingItsCallers) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // A threshold of 5: the caller's loop is compiled when it is entered after its
  // fifth poll, the callee tiers up from the interpreter's calls before that, so
  // by the time compiled code calls it the slot is filled.
  Scenario sc;
  sc.source =
      "function g(x) { return x * 2; }\n"
      "function f(n) { s = 0; i = 0; while (i < n) { s = s + g(i); i = i + 1; } return s; }\n"
      "print(g(1)); print(g(2)); print(g(3)); print(g(4)); print(g(5)); print(g(6));\n"
      "print(f(3)); print(f(3)); print(f(100));\n";
  Outcome plain = run(sc, 0);
  Outcome jit = run(sc, 5);
  EXPECT_EQ(plain.key(), jit.key());
  EXPECT_GE(jit.stats.calls, 100u);
  EXPECT_EQ(jit.stats.compile_at_call, 0u) << "g was compiled by the interpreter's tier-up, into the same slot";
  EXPECT_EQ(exits(jit.stats), 0u);
}

// A callee operand that two paths produce differently is not a call site: the
// operand stack position merges to "no single producer" and the CALL stays an exit
// (an unconditional one, so it counts against its function and not as a call exit),
// whatever the callee value is at run time. Without the merge the first path's
// producer would name the callee and the site would be compiled as a call to it,
// and the other path would leave at the callee-value guard instead.
TEST(JitCalls, ACalleeOperandTwoPathsProduceDifferentlyIsNeverACompiledCallSite) {
  GLTANG_REQUIRE_JIT_BACKEND();
  const std::vector<std::pair<const char *, const char *>> shapes = {
      {"ternary", "(c ? f : g)(x)"},
      {"and-or", "((c && f) || g)(x)"},
      {"nested ternary", "(c ? (x > 3 ? f : g) : g)(x)"},
  };
  for (const auto & shape : shapes) {
    const std::string callee = shape.second;
    auto program = [&](const std::string & call) {
      return "function f(x) { return x + 1; }\nfunction g(x) { return x + 2; }\n"
             "function t(c, x) { return " + call + "; }\n"
             "function drive(n) { s = 0; for (i = 0; i < n; i += 1) { s = s + t(i % 2 == 0, i); } return s; }\n"
             "print(drive(30));\n";
    };
    // The control: one named callee, a compiled call site.
    Scenario control;
    control.source = program("f(x)");
    control.script = true;
    Outcome control_plain, control_jit;
    expect_same(control, &control_plain, &control_jit);
    ASSERT_GE(control_jit.stats.calls, 20u) << shape.first << ": the control compiles its call";

    Scenario sc;
    sc.source = program(callee);
    sc.script = true;
    Outcome plain, jit;
    expect_same(sc, &plain, &jit);
    EXPECT_EQ(jit.raw, plain.raw) << shape.first;
    EXPECT_EQ(exits(jit.stats), 0u) << shape.first << ": the site was never a call site, so no call exit of any class was made at it (a callee-value guard exit means the merge was lost)";
    EXPECT_EQ(jit.stats.compile_at_call, 0u) << shape.first << ": no callee was compiled for a call site, there is none";
    EXPECT_GE(jit.stats.deopts, 1u) << shape.first << ": the CALL is an unconditional exit";
    EXPECT_LT(jit.stats.calls, control_jit.stats.calls) << shape.first << ": fewer compiled calls than the control, which has a site per iteration";
    EXPECT_EQ(jit.stats.rebuild_failures, 0u) << shape.first;
    EXPECT_EQ(jit.stats.hook_argument_errors, 0u) << shape.first;
    // Every function compiled: the analysis that merged the producers had to be run
    // again from nothing (the CALL was first walked with the first path's producer
    // and its successors with it), or the code after the exit is left reachable
    // from stale state and the compile of the function fails.
    EXPECT_EQ(jit.stats.compile_failures, 0u) << shape.first << ": a merge found after the CALL was walked must not leave the code after it half-analysed";
  }
}

TEST(JitCalls, ACalleeThatCannotBeCompiledIsAnExitOnceRememberedAndNeverCountsAgainstItsCaller) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // The caller is compiled; then the page provider stops making memory
  // executable, so the callee's compile fails at the first call from compiled
  // code. Its slot is refused and every later call exits at the cost of one
  // compare, without asking the compile hook again. The caller is entered 100
  // times and leaves at its call each time, and is never discarded.
  Scenario sc;
  sc.source =
      "function g(x) { return x + 1; }\n"
      "function caller(i) { return g(i) + 1; }\n"
      "t = 0; for (k = 0; k < 100; k += 1) { t = t + caller(k); } print(t);\n";
  sc.script = true;
  Context * seen = nullptr;
  sc.before = [&seen](Context & context) { seen = &context; };
  sc.on_poll = [&seen](Script &, GRCORE_Context *, uint64_t) {
    if (seen->jit_stats().functions_compiled >= 2) {
      seen->tracker.fail_protect = true;
    }
  };
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  EXPECT_EQ(jit.stats.functions_discarded, 0u) << "the exits at the call were not the caller's fault";
  EXPECT_GE(jit.stats.call_exits_remembered, 99u);
  EXPECT_EQ(jit.stats.call_exits_callee_guard, 0u);
  EXPECT_EQ(jit.stats.compile_hook_calls, 1u) << "the first call asked; the slot was then refused and later calls cost one compare and no hook";
  EXPECT_EQ(jit.stats.compile_at_call, 0u);
  EXPECT_EQ(jit.stats.compile_failures, 1u);
  EXPECT_EQ(jit.stats.calls, 0u);
}

TEST(JitCalls, ACallToAFunctionWithTooManyFrameSlotsIsAnUnconditionalExitLikeAnyOther) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // R3: a callee whose frame does not fit a compiled function is not called
  // through its slot at all; the call stays an exit, and an exit counts toward the
  // caller's discard limit like any other (only call exits that the callee or
  // the push caused do not).
  std::string locals;
  for (int i = 0; i < 1100; ++i) {
    locals += "  v" + std::to_string(i) + " = a + " + std::to_string(i % 7) + ";\n";
  }
  Scenario sc;
  sc.source =
      "function big(a) {\n" + locals + "  return v0 + v1099;\n}\n"
      "function caller(i) { return big(i) + 1; }\n"
      "t = 0; for (k = 0; k < 20; k += 1) { t = t + caller(k); } print(t);\n";
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  EXPECT_EQ(jit.stats.functions_discarded, 1u);
  EXPECT_EQ(exits(jit.stats), 0u) << "none of them was a call exit";
  EXPECT_EQ(jit.stats.calls, 0u);
}

TEST(JitCalls, ACalleeValueThatIsNotTheFunctionTheSiteNamesIsAGuardExitCountedAgainstTheCaller) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // A function name cannot be assigned, but a declaration can be skipped: `f` is
  // declared in a branch that is not taken, so the scan names it (from the
  // declaration) and the value is null. A mismatch is a guard exit, which counts
  // toward the caller's discard limit (the eighth discards `call`); the
  // interpreter makes the call and gives its error.
  Scenario sc;
  sc.source =
      "flag = false;\n"
      "if (flag) { function f(x) { return x + 1; } }\n"
      "function call(i) { return f(i); }\n"
      "t = 0; for (k = 0; k < 10; k += 1) { v = call(k); t = t + 1; } print(t);\n";
  sc.script = true;
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  EXPECT_EQ(jit.stats.call_exits_callee_guard, 8u);
  EXPECT_EQ(jit.stats.functions_discarded, 1u) << "a guard counts toward the caller's discard limit: the eighth exit discards it";
  EXPECT_EQ(jit.stats.call_exits_remembered + jit.stats.call_exits_push_refused, 0u);
  EXPECT_EQ(jit.stats.calls, 0u);
  EXPECT_EQ(jit.raw, "10");
  // The same function declared, and the guard holds.
  Scenario held = sc;
  held.source = "flag = true;\n" + sc.source.substr(sc.source.find('\n') + 1);
  Outcome plain2, jit2;
  expect_same(held, &plain2, &jit2);
  EXPECT_EQ(jit2.stats.call_exits_callee_guard, 0u);
  EXPECT_GE(jit2.stats.calls, 10u);
}

TEST(JitCalls, CallsTheScanCannotNameStayExitsWithTheInterpretersValueAndError) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc;
  sc.source =
      "use math.floor as floor;\n"
      "use t;\n"
      "function inc(x) { return x + 1; }\n"
      "function apply(f, x) { return f(x); }\n"
      "function viaarray(i) { fs = [inc, inc]; return fs[i % 2](i); }\n"
      "function wrong(i) { return inc(i, i); }\n"
      "function notfn(i) { x = 5; return x(i); }\n"
      "function native(i) { return floor(i); }\n"
      "function tmpl(i) { return t(); }\n"
      "for (k = 0; k < 12; k += 1) {\n"
      "  print(apply(inc, k)); print(viaarray(k)); print(wrong(k) as string); print(notfn(k) as string); print(native(k)); print(tmpl(k));\n"
      "}\n";
  Part part;
  part.name = "t";
  part.source = "T";
  sc.parts.push_back(part);
  sc.script = true;
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  EXPECT_EQ(plain.errors, jit.errors);
  EXPECT_EQ(jit.stats.hook_argument_errors, 0u);
  EXPECT_EQ(jit.stats.rebuild_failures, 0u);
}

// ---------------------------------------------------------------------------
// Deep chains with roots, a guard three frames down, pauses, unwinds
// ---------------------------------------------------------------------------

std::string deep_source(int depth) {
  return "function deep(n, a, b) {\n"
         "  if (n == 0) { i = 0; while (i < 3) { i = i + 1; } return a; }\n"
         "  r = deep(n - 1, b, a);\n"
         "  return r;\n"
         "}\n"
         "a = [1]; b = [2]; x = deep(" + std::to_string(depth) + ", a, b); print(x[0]); print(a[0]); print(b[0]);\n";
}

TEST(JitCalls, AChainOfSixtyFramesHoldingReferencesSurvivesACollectionAtItsBottomUnderTorture) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc;
  sc.source = deep_source(60);
  sc.script = true;
  sc.torture = 1;
  sc.verify = 1;
  sc.moving = 1;
  sc.on_poll = [](Script &, GRCORE_Context * context, uint64_t) {
    EXPECT_EQ(grheap_collect(grheap_heap_get(context)), GRHEAP_OK);
  };
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  EXPECT_EQ(jit.raw, "112");
  EXPECT_GE(jit.stats.deepest_chain, 60u);
  expect_clean_calls(jit, 60);
}

TEST(JitCalls, AChainOfFiveThousandFramesIsWalkedAndRebuilt) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // A stack that moves at every push copies itself 5,000 times, so the depth is
  // scaled down under the instruments that cost an order of magnitude. A collection
  // (a move, in the relocation arm) at every other poll while the chain is deep and
  // compiled: the compiled frames' references are updated through their stack maps
  // and nothing may put an older copy back (planted defect 20). No pause here: a
  // pause rebuilds the frames from the compiled ones and would hide that.
  const int depth = tt::heavy_instruments() ? 500 : 5000;
  Scenario sc;
  sc.source = deep_source(depth);
  sc.calls = 20000;
  sc.native_stack_bytes = uint64_t{4} << 20;
  sc.script = true;
  sc.on_poll = [](Script &, GRCORE_Context * context, uint64_t n) {
    if (n % 2 == 1) {
      EXPECT_EQ(grheap_collect(grheap_heap_get(context)), GRHEAP_OK);
    }
  };
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  EXPECT_EQ(jit.raw, "112") << "a reference put back over the one the collector updated reads as a poisoned cell, and the three prints are not 1, 1 and 2";
  EXPECT_GE(jit.stats.deepest_chain, static_cast<uint64_t>(depth) / 5);
}

TEST(JitCalls, AChainOfFiveThousandFramesPausedAtDepthIsRebuiltIntoItsGuestFramesAndFinishes) {
  GLTANG_REQUIRE_JIT_BACKEND();
  const int depth = tt::heavy_instruments() ? 500 : 5000;
  Scenario sc;
  sc.source = deep_source(depth);
  sc.calls = 20000;
  sc.native_stack_bytes = uint64_t{4} << 20;
  // The budget runs out while the chain is deep and is raised on each pause, so the
  // whole chain is walked and rebuilt into guest frames several times (a pause in
  // compiled code is refused and the frames are rebuilt), and the run resumes.
  sc.fuel = static_cast<uint64_t>(depth) * 3;
  sc.step = sc.fuel;
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  EXPECT_EQ(jit.raw, "112");
  EXPECT_GE(jit.stats.refused_pauses, 2u) << "a pause in compiled code rebuilt the chain, more than once";
  EXPECT_EQ(jit.stats.rebuild_failures, 0u);
}

TEST(JitCalls, AGuardThatFailsThreeFramesDownRebuildsTheChainAndTheCallersReenterCompiledCodeAtTheirNextCall) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc;
  sc.source =
      std::string("function g(n, x) { if (n == 0) { return x + x; } return g(n - 1, x) + 1; }\n"
                  "M = ") + kSourceMax + ";\n"
      "print(g(2, M)); print(g(2, 3)); print(g(2, M)); print(g(5, 4));\n";
  sc.script = true;
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  EXPECT_NE(plain.raw.find("1152921504606846976"), std::string::npos) << plain.raw;
  EXPECT_GE(jit.stats.deopts, 2u);
  EXPECT_GE(jit.stats.calls, 4u);
  EXPECT_EQ(jit.stats.last_exit_cause, 0u);
}

TEST(JitCalls, AFuelBudgetThatRunsOutInACalleeInTheCallerAfterACallAndExactlyAtACallPausesWhereTheInterpreterDoes) {
  GLTANG_REQUIRE_JIT_BACKEND();
  const char * source =
      "function leaf(x) { return x + 1; }\n"
      "function mid(x) { y = leaf(x); return leaf(y) + 1; }\n"
      "function top(n) { s = 0; i = 0; while (i < n) { s = s + mid(i); i = i + 1; } return s; }\n"
      "print(top(6));\n";
  // Every budget from 1 to 120 is a pause at a different place: inside `leaf`,
  // in `mid` after a call, in `top`, and exactly at a call.
  for (uint64_t budget = 1; budget <= 120; ++budget) {
    Scenario sc;
    sc.source = source;
    sc.fuel = budget;
    sc.step = budget < 7 ? 7 : budget;
    Outcome plain, jit;
    expect_same(sc, &plain, &jit);
    ASSERT_EQ(plain.pauses, jit.pauses) << "budget " << budget;
    ASSERT_EQ(plain.fuel, jit.fuel) << "budget " << budget;
    ASSERT_EQ(plain.raw, jit.raw) << "budget " << budget;
  }
}

TEST(JitCalls, APauseFiftyFramesDownLeavesOnlyInterpreterFramesAndResumesToTheUninterruptedOutput) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc;
  sc.source =
      "function down(n) { if (n == 0) { i = 0; while (i < 400) { i = i + 1; } return i; } return down(n - 1) + 1; }\n"
      "print(down(50)); print(down(50));\n";
  sc.fuel = 1300;
  sc.step = 500;
  sc.script = true;
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  ASSERT_GT(plain.pauses.size(), 3u);
  EXPECT_EQ(plain.pauses, jit.pauses);
  EXPECT_EQ(jit.raw, "450" "450");
  EXPECT_GE(jit.stats.refused_pauses, 2u);
  EXPECT_EQ(jit.stats.last_exit_cause, 1u);
  EXPECT_GE(jit.stats.deepest_chain, 50u) << "the first pause is at the bottom of a chain of fifty";
}

TEST(JitCalls, AChainPausedFiftyFramesDownResumesOnAnotherThread) {
  GLTANG_REQUIRE_JIT_BACKEND();
  const char * source =
      "function down(n) { if (n == 0) { i = 0; while (i < 600) { i = i + 1; } return i; } return down(n - 1) + 1; }\n"
      "t = 0; for (k = 0; k < 6; k += 1) { t = t + down(50); } print(t);\n";
  Compiled compiled(source, Mode::Script, "hop.tang");
  ASSERT_TRUE(compiled.ok());
  Config reference_config;
  reference_config.jit_threshold = 0;
  Context reference(compiled.program, reference_config);
  ASSERT_TRUE(reference.ok());
  ASSERT_TRUE(reference.execute());
  Config config;
  config.fuel = 1500;
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
      grcore_context_set_fuel(context.context, grcore_context_fuel_used(context.context) + 1500);
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
  EXPECT_GT(context.jit_stats().refused_pauses, 2u);
  EXPECT_GE(context.jit_stats().deepest_chain, 50u);
  EXPECT_EQ(context.jit_stats().hook_argument_errors, 0u);
  // The paused context holds interpreter frames only: the last pause left no
  // compiled frame to walk.
  GRCORE_CompiledWalk walk;
  GRCORE_CompiledFrame frame;
  ASSERT_EQ(grcore_compiled_walk_begin(context.context, &walk), GRCORE_OK);
  EXPECT_EQ(grcore_compiled_walk_next(&walk, &frame), GRCORE_CWALK_END);
}

TEST(JitCalls, ARuntimeLimitInsideACompiledChainUnwindsLikeTheInterpretersWithNoFrameLeft) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // A template's budget runs out while a chain of its recursion is compiled: the
  // scope's value, the error list entry and the template chain are the
  // interpreter's, and no compiled frame is converted (an unwind pops them).
  Scenario sc;
  sc.source = "use t; print(\"[\" + t() + \"]\"); print(\"after\");";
  Part t;
  t.name = "t";
  t.source =
      "function spin(n) { k = 0; while (k < n) { k += 1; } return k; }\n"
      "function r(n) { if (n == 0) { return spin(100000); } return r(n - 1) + 1; }\n"
      "print(r(20));";
  t.fuel = 900;
  t.policy = GLTANG_SCOPE_EMPTY;
  sc.parts.push_back(t);
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  EXPECT_GE(jit.stats.refused_unwinds, 1u);
  EXPECT_EQ(jit.stats.last_exit_cause, 2u);
  EXPECT_FALSE(jit.errors.empty());
  EXPECT_NE(jit.raw.find("after"), std::string::npos);
}

TEST(JitCalls, ATerminateRequestInsideACompiledChainEndsTheRunLikeTheInterpretersOwn) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc;
  sc.source =
      "function down(n) { if (n == 0) { i = 0; while (i < 100000) { i = i + 1; } return i; } return down(n - 1) + 1; }\n"
      "print(down(30));";
  sc.script = true;
  sc.on_poll = [](Script & script, GRCORE_Context *, uint64_t n) {
    if (n == 60) {
      script.post(GRCORE_REQUEST_TERMINATE);
    }
  };
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  EXPECT_FALSE(jit.finished);
  EXPECT_NE(jit.ran, GRCORE_OK);
  EXPECT_EQ(jit.stats.refused_unwinds, 1u);
  EXPECT_EQ(jit.stats.last_exit_cause, 2u);
}

// ---------------------------------------------------------------------------
// Budgets: the guest depth, the native stack, memory
// ---------------------------------------------------------------------------

TEST(JitCalls, RecursionPastTheGuestDepthBudgetGivesTheSameErrorAtTheSamePollWithTheSameFuel) {
  GLTANG_REQUIRE_JIT_BACKEND();
  for (uint64_t budget : {uint64_t{1}, uint64_t{2}, uint64_t{50}, uint64_t{1000}}) {
    Scenario sc;
    sc.source =
        "function f(n) { return f(n + 1); }\n"
        "function g(n) { if (n > 0) { return g(n - 1) + 1; } return 0; }\n"
        "x = f(0); print(x as string); print(g(40) as string);\n";
    sc.calls = budget;
    sc.script = true;
    Outcome plain, jit;
    expect_same(sc, &plain, &jit);
    EXPECT_EQ(plain.fuel, jit.fuel) << "budget " << budget;
    EXPECT_EQ(plain.polls, jit.polls) << "budget " << budget;
    EXPECT_EQ(plain.errors, jit.errors) << "budget " << budget;
    if (budget >= 50) {
      // f(0) goes down to the budget: the top level, f's entry from the interpreter
      // and budget - 1 compiled calls; then g(40) makes 40 more. A depth test
      // that is off by one in either direction shows here at the first budget.
      EXPECT_EQ(jit.stats.calls, budget - 1 + 40) << "budget " << budget;
      EXPECT_GE(jit.stats.call_exits_push_refused, 1u) << "budget " << budget << ": the refused push is an exit, and the interpreter makes the call and gets the error";
    }
    EXPECT_EQ(jit.stats.hook_argument_errors, 0u);
    if (HasFailure()) {
      return;
    }
  }
}

TEST(JitCalls, ATinyNativeStackDeoptimizesTheChainAtTheCallAndTheInterpreterFinishesWithTheInterpretedOutput) {
  GLTANG_REQUIRE_JIT_BACKEND();
  const char * source =
      "function d(n) { if (n <= 0) { return 0; } return d(n - 1) + 1; }\n"
      "print(d(3000)); print(d(10)); print(d(3000));\n";
  Scenario base;
  base.source = source;
  base.calls = 10000;
  Outcome plain = run(base, 0);
  uint64_t previous = UINT64_MAX;
  // 512 and 16,384 bytes do not even hold the interpreter's own frames and one
  // compiled function: every entry leaves at its first stack check, without a
  // compiled frame to rebuild, and the interpreter runs the function. 100,000
  // holds about 370 compiled frames (about 270 bytes each), so the chain leaves at
  // the call that would pass the limit.
  for (uint64_t bytes : {uint64_t{512}, uint64_t{16384}, uint64_t{100000}, uint64_t{1} << 20, uint64_t{16} << 20}) {
    Scenario sc = base;
    sc.native_stack_bytes = bytes;
    Outcome jit = run(sc, 1);
    EXPECT_EQ(plain.key(), jit.key()) << "native stack " << bytes;
    EXPECT_EQ(plain.polls, jit.polls);
    EXPECT_EQ(jit.stats.hook_argument_errors, 0u);
    if (bytes == 100000) {
      EXPECT_GE(jit.stats.call_exits_native_stack, 1u) << "native stack " << bytes << ": below the need the chain deoptimizes at the call";
      EXPECT_GE(jit.stats.deepest_chain, 100u);
    }
    if (bytes <= 16384) {
      EXPECT_EQ(jit.stats.calls, 0u) << "native stack " << bytes << ": nothing is entered";
    }
    if (bytes >= (uint64_t{16} << 20)) {
      EXPECT_EQ(jit.stats.call_exits_native_stack, 0u);
    }
    // More room, more depth: the deepest chain does not shrink as the budget grows.
    EXPECT_LE(previous == UINT64_MAX ? 0 : previous, jit.stats.deepest_chain) << "native stack " << bytes;
    previous = jit.stats.deepest_chain;
  }
}

TEST(JitCalls, AFramePushedForACalleeWhoseEntryThenFailsTheStackCheckHoldsItsReferenceArgumentsAfterTheCollectionThePushCaused) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // The push is a GC point. When the callee's entry then fails its native stack
  // check there is no compiled frame to rebuild: the interpreter runs the callee
  // from the guest frame the push made, so the arguments the hook wrote there must
  // be the ones read after the collection (and, in the relocation arm, the move),
  // not copies taken before it.
  //
  // A relocating heap moves every object at every collection and an object put
  // back where it was by a second move would hide a stale copy, so the window is
  // narrow: the failing push must be the last collection before the first use. The
  // push hook collects at the first 256 levels of the guest stack and at every
  // 1,024th call, so the budgets are swept across the one where the 1,024th call
  // is the push whose callee does not fit (about 380 bytes a compiled frame with these six parameters), and
  // the recursion reads its arguments at the bottom, with no other collection
  // between. Torture is off for the same reason: the push hook's collection is the
  // only one.
  // Under a stack that moves at every push, or Valgrind, the sweep is coarse and
  // the narrow window is left to the plain run (the planted-defect check runs that).
  const uint64_t step = (tt::moving_stack_requested() || RUNNING_ON_VALGRIND) ? 2048 : 64;
  uint64_t left_at_a_call = 0, runs = 0;
  for (uint64_t bytes = 360000; bytes <= 392000; bytes += step) {
    ++runs;
    Scenario sc;
    sc.source =
        "function f(a, b, c, d, e, n) { if (n <= 0) { return a[0] + b[0] + c[0] + d[1] + e[3]; } return f(a, b, c, d, e, n - 1); }\n"
        "a = [7]; b = [8, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]; c = [1, 2, 3, 4, 5, 6, 7, 8, 9]; d = [0, 20]; e = [0, 0, 0, 5];\n"
        "print(f(a, b, c, d, e, 1300)); print(a[0] * b[0]);\n";
    sc.script = true;
    sc.calls = 4000;
    sc.torture = 0;
    sc.verify = 0;
    sc.gc_at_push = true;
    sc.native_stack_bytes = bytes;
    Outcome plain, jit;
    expect_same(sc, &plain, &jit);
    EXPECT_EQ(jit.raw, "4156") << "native stack " << bytes;
    if (jit.stats.call_exits_native_stack >= 1u) {
      ++left_at_a_call;
    }
    if (HasFailure()) {
      return;
    }
  }
  EXPECT_EQ(left_at_a_call, runs) << "every budget leaves the chain at the call that does not fit";
}

TEST(JitCalls, WithNoNativeStackBudgetNoCallSiteIsCompiledAndDepthOneHundredThousandDoesNotFault) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // A stack that moves at every push copies itself at every call, so the depth is
  // scaled down under the instruments that cost an order of magnitude; plain, it
  // is 100,000.
  const int depth = tt::heavy_instruments() ? 5000 : 100000;
  Scenario sc;
  sc.source =
      "function d(n) { if (n <= 0) { return 0; } return d(n - 1) + 1; }\n"
      "print(d(" + std::to_string(depth) + "));\n";
  sc.calls = 200000;
  sc.native_stack_bytes = GRCORE_UNLIMITED;
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  EXPECT_EQ(jit.raw, std::to_string(depth));
  EXPECT_EQ(jit.stats.calls, 0u) << "a context with no byte budget compiles no call sites";
  EXPECT_EQ(exits(jit.stats), 0u);
}

TEST(JitCalls, TheMemoryBudgetSweptAcrossTheByteWhereTheGuestStackMustGrowGivesTheSameVerdictOnBothTiersAtEveryBudget) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc;
  sc.source =
      "function d(n) { if (n <= 0) { return 0; } return d(n - 1) + 1; }\n"
      "x = d(40); print(x as string); y = d(40); print(y as string);\n";
  sc.memory_reserve = 0;
  Outcome measured = run(sc, 0);
  ASSERT_TRUE(measured.created);
  ASSERT_GT(measured.memory_peak, 0u);
  // The executable pages of compiled code are the engine's, not the program's, and
  // are not charged to the guest's memory budget (they are counted in the JIT
  // statistics), so the tiers give the same verdict at every budget: the run that
  // finishes, or the limit error, in the same place. The sweep covers both peaks.
  Outcome compiled_measured = run(sc, 1);
  ASSERT_TRUE(compiled_measured.created);
  EXPECT_GT(compiled_measured.stats.code_bytes_mapped, 0u) << "the compiled run mapped executable pages, and they are visible in the statistics";
  const uint64_t low = std::min(measured.memory_peak, compiled_measured.memory_peak) - 1500;
  const uint64_t high = std::max(measured.memory_peak, compiled_measured.memory_peak) + 1500;
  std::set<std::string> kinds;
  for (uint64_t i = 0; i < 30; ++i) {
    Scenario tight = sc;
    tight.memory_bytes = low + (high - low) * i / 29;
    Outcome plain = run(tight, 0);
    Outcome jit = run(tight, 1);
    kinds.insert(plain.key());
    EXPECT_EQ(plain.key(), jit.key()) << "memory " << tight.memory_bytes;
    EXPECT_EQ(plain.polls, jit.polls) << "memory " << tight.memory_bytes;
  }
  EXPECT_GE(kinds.size(), 2u) << "the sweep saw only one outcome, so it did not cross the budget it is meant to";
  // The probe is not trivial: the guest stack starts small and grew while the
  // chain was built, in the interpreter and in the compiled run.
  EXPECT_GT(measured.memory_peak, 2000u);
  EXPECT_GT(measured.stack_moves, 0u) << "the guest stack grew";
  EXPECT_GT(run(sc, 1).stack_moves, 0u);
}

// ---------------------------------------------------------------------------
// Exit causes, a refused rebuild, registered code, discard, forged installs
// ---------------------------------------------------------------------------

TEST(JitCalls, EveryExitClassArrivesWithItsOwnCauseAndIsHandledByName) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // A guard.
  {
    Scenario sc;
    sc.source = std::string("function g(x) { return x + x; }\nM = ") + kSourceMax + ";\nprint(g(M));";
    Outcome jit = run(sc, 1);
    EXPECT_GE(jit.stats.deopts, 1u);
    EXPECT_EQ(jit.stats.last_exit_cause, 0u);
  }
  // A call exit (a callee that cannot be compiled: the page provider stops making
  // memory executable once both functions are compiled, so the callee's compile at
  // its first call from compiled code fails and the site is classified as a
  // remembered exit).
  {
    Scenario sc;
    sc.source = "function h(x) { return x + 1; }\nfunction g(x) { return h(x) + 1; }\nt = 0; for (k = 0; k < 20; k += 1) { t = t + g(k); } print(t);";
    sc.script = true;
    Context * seen = nullptr;
    sc.before = [&seen](Context & context) { seen = &context; };
    sc.on_poll = [&seen](Script &, GRCORE_Context *, uint64_t) {
      if (seen->jit_stats().functions_compiled >= 2) {
        seen->tracker.fail_protect = true;
      }
    };
    Outcome jit = run(sc, 1);
    EXPECT_GE(jit.stats.call_exits_remembered, 1u);
    EXPECT_GE(jit.stats.deopts, 1u);
    EXPECT_EQ(jit.stats.last_exit_cause, 0u);
    EXPECT_EQ(jit.raw, "230");
  }
  // A pause.
  {
    Scenario sc;
    sc.source = "function g(n) { i = 0; while (i < n) { i = i + 1; } return i; }\nprint(g(1000));";
    sc.fuel = 200;
    sc.step = 0;
    Outcome jit = run(sc, 1);
    EXPECT_EQ(jit.stats.refused_pauses, 1u);
    EXPECT_EQ(jit.stats.last_exit_cause, 1u);
  }
  // An unwind.
  {
    Scenario sc;
    sc.source = "function g(n) { i = 0; while (i < n) { i = i + 1; } return i; }\nprint(g(100000));";
    sc.script = true;
    sc.on_poll = [](Script & script, GRCORE_Context *, uint64_t n) {
      if (n == 10) {
        script.post(GRCORE_REQUEST_TERMINATE);
      }
    };
    Outcome jit = run(sc, 1);
    EXPECT_EQ(jit.stats.refused_unwinds, 1u);
    EXPECT_EQ(jit.stats.last_exit_cause, 2u);
  }
  // The native stack.
  {
    Scenario sc;
    sc.source = "function d(n) { if (n <= 0) { return 0; } return d(n - 1) + 1; }\nprint(d(1500));";
    sc.calls = 5000;
    sc.native_stack_bytes = 100000;
    Outcome jit = run(sc, 1);
    EXPECT_GE(jit.stats.call_exits_native_stack, 1u);
    EXPECT_EQ(jit.stats.last_exit_cause, 0u);
  }
}

TEST(JitCalls, ARefusedRebuildAtTheEntryEndsTheRunUnwoundAndIsCountedNeverContinued) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // The first exit of the run is the entry function's own, with one compiled
  // frame to rebuild; the injected failure refuses it. The hook's code is the
  // entry's REBUILD_FAILED, the run ends unwound without interpreting the frame,
  // and the failure is counted. A hook that ignored the refusal would return
  // DEOPTED, and the stat and the output would say so.
  Scenario sc;
  sc.source = "print(\"before\"); function g(x) { return x + 1; } print(g(2)); print(\"never\");\n";
  sc.fail_rebuild = true;
  Outcome jit = run(sc, 1);
  ASSERT_TRUE(jit.created);
  EXPECT_FALSE(jit.finished);
  EXPECT_NE(jit.ran, GRCORE_OK) << "the run ended unwound";
  EXPECT_EQ(jit.stats.rebuild_failures, 1u);
  EXPECT_NE(jit.stats.last_exit_cause, 0u) << "the hook's own code, not a poll's answer";
  EXPECT_EQ(jit.stats.deopts, 0u) << "an exit whose rebuild was refused is not a deoptimization";
  EXPECT_EQ(jit.raw.find("never"), std::string::npos) << "nothing ran on the frames that were not rebuilt";
  EXPECT_EQ(jit.native_depth, 0u);
  // The control: the same program without the injection finishes.
  Scenario control = sc;
  control.fail_rebuild = false;
  Outcome ok = run(control, 1);
  EXPECT_TRUE(ok.finished);
  EXPECT_EQ(ok.stats.rebuild_failures, 0u);
}

TEST(JitCalls, ARefusedRebuildInADeepChainPopsEveryFrameItPushedAndLeavesNothingBehind) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // The failure is injected at the fifteenth poll, while a chain of compiled
  // frames is on the stack: the guard that fails at its bottom starts a rebuild
  // that is refused. Every guest frame the run pushed is popped, the context is
  // destroyed cleanly (the harness checks every block and page), and nothing
  // after the failure runs.
  Scenario sc;
  sc.source =
      std::string("function g(n, x) { if (n == 0) { i = 0; while (i < 10) { i = i + 1; } return x + x; } return g(n - 1, x) + 1; }\nM = ") + kSourceMax + ";\n"
      "print(\"before\"); print(g(12, M)); print(\"never\");\n";
  sc.script = true;
  Context * seen = nullptr;
  sc.before = [&seen](Context & context) { seen = &context; };
  sc.on_poll = [&seen](Script &, GRCORE_Context *, uint64_t n) {
    if (n == 15) {
      gltang_vm_set_jit_test_switches_unchecked(seen->execution, false, true, false);
    }
  };
  Outcome jit = run(sc, 1);
  ASSERT_TRUE(jit.created);
  EXPECT_FALSE(jit.finished);
  EXPECT_EQ(jit.stats.rebuild_failures, 1u);
  EXPECT_EQ(jit.raw.find("never"), std::string::npos);
  EXPECT_GE(jit.stats.deepest_chain, 10u) << "the failure was in a chain, not at its entry";
  EXPECT_EQ(jit.native_depth, 0u);
}

TEST(JitCalls, EveryCompiledRangeIsRegisteredWhileAFrameCanReturnIntoIt) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc;
  sc.source =
      "function leaf(n) { i = 0; while (i < n) { i = i + 1; } return i; }\n"
      "function mid(n) { return leaf(n) + 1; }\n"
      "function top(n) { return mid(n) + mid(n); }\n"
      "print(top(30)); print(top(30));\n";
  sc.script = true;
  Context * seen = nullptr;
  uint64_t checks = 0;
  sc.before = [&seen](Context & context) { seen = &context; };
  sc.on_poll = [&seen, &checks](Script &, GRCORE_Context * context, uint64_t) {
    GLTANG_JitStats st = seen->jit_stats();
    EXPECT_EQ(grcore_code_registered_count(context), st.functions_compiled - st.functions_discarded);
    GRCORE_CompiledWalk walk;
    GRCORE_CompiledFrame frame;
    if (grcore_compiled_walk_begin(context, &walk) == GRCORE_OK) {
      while (grcore_compiled_walk_next(&walk, &frame) == GRCORE_CWALK_FRAME) {
        GRCORE_CodeRange range;
        EXPECT_TRUE(grcore_code_lookup(context, frame.return_address, &range));
        ++checks;
      }
    }
  };
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  EXPECT_GT(checks, 10u) << "frames were found on the stack at the polls";
}

TEST(JitCalls, TheEighthExitOfAFunctionDiscardsItByReferenceAndLaterCallsAreTheInterpretersOwn) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc;
  sc.source =
      std::string("function r(n, x) { if (n == 0) { return x + x; } return r(n - 1, x) + 1; }\nM = ") + kSourceMax + ";\n"
      "t = 0; for (k = 0; k < 14; k += 1) { y = r(3, M); t = t + 1; } print(t); print(r(2, 5));\n";
  sc.script = true;
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  EXPECT_EQ(jit.stats.functions_discarded, 1u);
  EXPECT_EQ(jit.stats.deopts, 9u) << "the top level once, and r eight times: the eighth discards it and the ninth call is the interpreter's";
  EXPECT_EQ(jit.raw, "14" "12");
}

TEST(JitCalls, PausesInOneHotFunctionNeverDiscardItWhileGuardExitsStillDoAtTheEighth) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // A poll's deopt is not a guard: a debugger step or a fuel pause must not throw
  // away hot code (decided at the done checkpoint). Thirty calls of one function,
  // the budget raised by a small step on every pause, so many pauses are taken
  // inside its compiled loop. (A function can be unwound only once, the unwind ends
  // the run, so the unwind cause is checked as one more exit that does not count.)
  {
    Scenario sc;
    sc.source =
        "function g(n) { i = 0; while (i < n) { i = i + 1; } return i; }\n"
        "t = 0; for (k = 0; k < 30; k += 1) { t = t + g(150); } print(t);\n";
    sc.script = true;
    sc.fuel = 700;
    sc.step = 700;
    Outcome plain, jit;
    expect_same(sc, &plain, &jit);
    EXPECT_EQ(jit.raw, "4500");
    EXPECT_GE(jit.stats.refused_pauses, 9u) << "more pauses than the discard limit were taken in compiled code";
    EXPECT_EQ(jit.stats.functions_discarded, 0u) << "pauses do not count toward the discard limit";
    EXPECT_EQ(jit.registered, jit.stats.functions_compiled) << "every compiled function is still registered";
    EXPECT_GE(jit.stats.entries, 20u) << "later calls kept entering compiled code";
  }
  {
    Scenario sc;
    sc.source = "function g(n) { i = 0; while (i < n) { i = i + 1; } return i; }\nt = 0; for (k = 0; k < 9; k += 1) { t = t + g(20000); } print(t);\n";
    sc.script = true;
    sc.on_poll = [](Script & script, GRCORE_Context *, uint64_t n) {
      if (n == 3000) {
        script.post(GRCORE_REQUEST_TERMINATE);
      }
    };
    Outcome jit = run(sc, 1);
    EXPECT_EQ(jit.stats.refused_unwinds, 1u);
    EXPECT_EQ(jit.stats.functions_discarded, 0u) << "an unwind does not count toward the discard limit";
  }
  // The control: the same eight-exit limit, reached by guard exits, still discards.
  {
    Scenario sc;
    sc.source = std::string("function g(x) { return x + x; }\nM = ") + kSourceMax + ";\nt = 0; for (k = 0; k < 12; k += 1) { y = g(M); t = t + 1; } print(t);\n";
    sc.script = true;
    Outcome plain, jit;
    expect_same(sc, &plain, &jit);
    EXPECT_EQ(jit.stats.functions_discarded, 1u) << "guard exits still discard a function at the eighth";
  }
}

/// GLTANG_LONG_RUN_FUNCTIONS=N: how many functions the thrashing program has
/// (1,000 on the EVO; a small number in `make test`).
int thrash_functions() {
  const char * v = std::getenv("GLTANG_LONG_RUN_FUNCTIONS");
  int n = v && *v ? std::atoi(v) : 0;
  return n > 0 ? n : (tt::heavy_instruments() ? 12 : 40);
}

TEST(JitCalls, ARetiredCodeListStaysWithinTheDiscardedFunctionsOfALongCompiledRun) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // A thrashing generated program: many functions, each called from a compiled
  // loop and each discarded once by a guard that fails eight times, all under one
  // run. Every discarded function retires its code once and is never compiled
  // again, so the claim is that the retired list is bounded by the program's
  // functions: its peak is read from the run and must not pass twice the number
  // discarded. (If it does, the story stops: a bound or an epoch is runtime-core's
  // design, not this engine's to decide.)
  const int kFunctions = thrash_functions();
  std::string source = "M = " + std::string(kSourceMax) + ";\n";
  for (int i = 0; i < kFunctions; ++i) {
    source += "function f" + std::to_string(i) + "(x) { return x + x; }\n";
  }
  // `drive` is split in groups so that no function has more locals than a frame fits.
  const int kGroup = 40;
  for (int g = 0; g * kGroup < kFunctions; ++g) {
    source += "function drive" + std::to_string(g) + "(i) { s = 0;\n";
    for (int i = g * kGroup; i < kFunctions && i < (g + 1) * kGroup; ++i) {
      source += "  s = s + f" + std::to_string(i) + "(i);\n";
    }
    source += "  return s; }\n";
  }
  source += "for (k = 0; k < 12; k += 1) {\n";
  for (int g = 0; g * kGroup < kFunctions; ++g) {
    source += "  drive" + std::to_string(g) + "(M);\n";
  }
  source += "}\nprint(drive0(1));\n";
  Scenario sc;
  sc.source = source;
  sc.fuel = 2000000000;
  Outcome plain = run(sc, 0);
  Outcome jit = run(sc, 1);
  ASSERT_TRUE(jit.finished) << "the thrashing program runs to its end";
  EXPECT_EQ(plain.key(), jit.key());
  EXPECT_GE(jit.stats.functions_discarded, static_cast<uint64_t>(kFunctions) / 2);
  // Measured 0 (on the EVO with 1,000 functions and here with 40): a discard is
  // applied after the entry's record is left, so core frees the retired code at
  // once. The bound is therefore zero, not the 2x the discards the claim allows; a
  // peak above zero would mean a discard made while a record was open, which this
  // program never does (a nested activation is story 9's case).
  EXPECT_EQ(jit.retired_peak, 0u)
      << "the retired list stays empty (peak " << jit.retired_peak << " for " << jit.stats.functions_discarded << " discarded)";
  std::printf("  thrashing program: %d functions, %llu discarded, retired peak %llu, %llu ranges registered at the end\n", kFunctions,
      (unsigned long long)jit.stats.functions_discarded, (unsigned long long)jit.retired_peak, (unsigned long long)jit.registered);
}

TEST(JitCalls, ALongCompiledRunOfFibRetiresNothingAndKeepsEveryCompiledRangeRegistered) {
  GLTANG_REQUIRE_JIT_BACKEND();
  const char * n = std::getenv("GLTANG_LONG_RUN_FIB");
  int depth = n && *n ? std::atoi(n) : (tt::heavy_instruments() ? 14 : 22);
  Scenario sc;
  sc.source = "function fib(n) { if (n < 2) { return n; } return fib(n - 1) + fib(n - 2); }\nprint(fib(" + std::to_string(depth) + "));\n";
  sc.calls = 1000;
  sc.fuel = 100000000000ull;
  Outcome jit = run(sc, 1);
  ASSERT_TRUE(jit.finished);
  EXPECT_GT(jit.stats.calls, 1000u);
  EXPECT_EQ(jit.stats.functions_discarded, 0u);
  EXPECT_EQ(jit.retired_peak, 0u) << "nothing was replaced or discarded, so nothing was retired";
  EXPECT_EQ(jit.registered, jit.stats.functions_compiled);
  std::printf("  fib(%d): %llu compiled calls, %llu call exits, retired peak %llu\n", depth, (unsigned long long)jit.stats.calls,
      (unsigned long long)exits(jit.stats), (unsigned long long)jit.retired_peak);
}

TEST(JitCalls, AnInstallOfCodeBuiltForAnotherFunctionOrCountIsRefusedAndTheSlotIsUnchanged) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Compiled compiled(
      "function two(a, b) { return a + b; }\nfunction three(a, b, c) { return a + b + c; }\n"
      "function run() { return two(1, 2) + three(1, 2, 3); }\nprint(run());",
      Mode::Script, "forge.tang");
  ASSERT_TRUE(compiled.ok());
  Config config;
  config.jit_threshold = 1;
  Context context(compiled.program, config);
  ASSERT_TRUE(context.ok());
  ASSERT_TRUE(context.execute());
  ASSERT_EQ(context.raw(), "9");
  // two = function 1, three = function 2: both compiled, both with slots. Offer
  // each one's code to the other's slot, and to a slot with the wrong count.
  for (int mode = 0; mode < 3; ++mode) {
    uintptr_t before_two = 0, after_two = 0;
    int refused = gltang_vm_jit_test_forged_install(context.execution, 2, 1, mode, &before_two, &after_two);
    EXPECT_EQ(refused, 1) << "mode " << mode << ": the forged install was refused";
    EXPECT_NE(before_two, 0u);
    EXPECT_EQ(before_two, after_two) << "mode " << mode << ": the slot is unchanged";
  }
  // The control: the right code with the right token and count installs.
  uintptr_t before = 0, after = 0;
  EXPECT_EQ(gltang_vm_jit_test_forged_install(context.execution, 1, 1, 3, &before, &after), 0);
}

TEST(JitCalls, AHookRefusesAnArgumentItDoesNotAcceptAndCountsIt) {
  // No backend is needed: the hooks check their arguments before they touch
  // anything, and nothing is compiled.
  Compiled compiled("function two(a, b) { return a + b; }\nprint(two(1, 2));", Mode::Script, "hooks.tang");
  ASSERT_TRUE(compiled.ok());
  Config config;
  config.jit_threshold = 1;
  Context context(compiled.program, config);
  ASSERT_TRUE(context.ok());
  GLTANG_Execution * exec = context.execution;
  const uint64_t two = (uint64_t{0} << 32) | 1u;  // program 0, function 1: two(a, b), so three words with the flag
  const uint64_t ok_args[3] = {(uint64_t{1} << 4) | 1u, (uint64_t{2} << 4) | 1u, 0};
  const uint64_t flag_set[3] = {(uint64_t{1} << 4) | 1u, (uint64_t{2} << 4) | 1u, 1};
  uint64_t before = context.jit_stats().hook_argument_errors;
  EXPECT_EQ(before, 0u);
  // The wrong count (one too few, one too many, none), the flag set (an interpreter
  // entry's, which a compiled call never passes), and a token that names no
  // function (past the program's functions, and in a program that is not loaded).
  EXPECT_NE(gltang_vm_jit_test_hook(exec, 0, two, ok_args, 2), 0);
  EXPECT_NE(gltang_vm_jit_test_hook(exec, 0, two, ok_args, 4), 0);
  EXPECT_NE(gltang_vm_jit_test_hook(exec, 0, two, ok_args, 0), 0);
  EXPECT_NE(gltang_vm_jit_test_hook(exec, 0, two, flag_set, 3), 0);
  EXPECT_NE(gltang_vm_jit_test_hook(exec, 0, (uint64_t{0} << 32) | 77u, ok_args, 3), 0);
  EXPECT_NE(gltang_vm_jit_test_hook(exec, 0, (uint64_t{5} << 32) | 1u, ok_args, 3), 0);
  EXPECT_EQ(context.jit_stats().hook_argument_errors, 6u) << "each refusal was counted";
  EXPECT_EQ(grcore_stack_frame_count(grcore_context_stack(context.context)), 0u) << "and none pushed a frame";
  // The compile hook refuses a token that names nothing, and a function that was
  // never named is compiled by it (counted as compiled at a call).
  EXPECT_NE(gltang_vm_jit_test_hook(exec, 2, (uint64_t{5} << 32) | 1u, nullptr, 0), 0);
  EXPECT_NE(gltang_vm_jit_test_hook(exec, 2, (uint64_t{0} << 32) | 99u, nullptr, 0), 0);
}

TEST(JitCalls, TheEmittedMetadataHasNoDerivedPointerAndNoConvertingLocation) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Compiled compiled(
      "function f(a, b) { s = 0; i = 0; while (i < a) { s = s + g(i, b); i = i + 1; } return s; }\n"
      "function g(x, y) { if (x < y) { return x; } return y; }\nprint(f(10, 5)); print(f(10, 7));",
      Mode::Script, "meta.tang");
  ASSERT_TRUE(compiled.ok());
  Config config;
  config.jit_threshold = 1;
  Context context(compiled.program, config);
  ASSERT_TRUE(context.ok());
  ASSERT_TRUE(context.execute());
  uint64_t sites = 0, derived = 0, converting = 0;
  ASSERT_EQ(gltang_vm_jit_test_metadata(context.execution, &sites, &derived, &converting), 1);
  EXPECT_GT(sites, 10u);
  EXPECT_EQ(derived, 0u) << "lang-tang has no derived pointers";
  EXPECT_EQ(converting, 0u) << "and no converting representation, which is why the reservation is always zero";
}

// ---------------------------------------------------------------------------
// Generated call graphs
// ---------------------------------------------------------------------------

TEST(JitCalls, GeneratedCallGraphsRunToTheSameVerdictWithEveryFunctionCompiledAtItsFirstPollAndTheFrameDifferentialShowsNoExitAtACompiledCall) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // GLTANG_CALL_GRAPH_SEEDS=N and GLTANG_CALL_GRAPH_FIRST=S run N programs from seed S
  // (the soak on the EVO runs tens of thousands, under GC torture and a moving stack).
  const char * seeds_text = std::getenv("GLTANG_CALL_GRAPH_SEEDS");
  const char * first_text = std::getenv("GLTANG_CALL_GRAPH_FIRST");
  const uint64_t kSeeds = seeds_text && *seeds_text ? std::strtoull(seeds_text, nullptr, 10) : (tt::heavy_instruments() ? 60 : 300);
  const uint64_t kFirst = first_text && *first_text ? std::strtoull(first_text, nullptr, 10) : 1;
  uint64_t calls = 0, compiled_programs = 0, call_exit_total = 0, compared = 0;
  for (uint64_t seed = kFirst; seed < kFirst + kSeeds; ++seed) {
    Scenario sc;
    sc.source = gen::generate_calls(seed).source;
    sc.fuel = 20000000;
    sc.calls = 512;
    sc.script = true;
    Outcome plain, jit;
    expect_same(sc, &plain, &jit);
    ASSERT_FALSE(HasFailure()) << "seed " << seed << ":\n" << sc.source;
    calls += jit.stats.calls;
    compiled_programs += jit.stats.calls > 0;
    call_exit_total += exits(jit.stats);
    ++compared;
    EXPECT_EQ(jit.stats.hook_argument_errors, 0u) << "seed " << seed;
    EXPECT_EQ(jit.stats.rebuild_failures, 0u) << "seed " << seed;
  }
  EXPECT_EQ(compared, kSeeds);
  EXPECT_GT(calls, kSeeds * 20) << "the comparison is not vacuous: compiled calls were made";
  EXPECT_GT(compiled_programs, kSeeds / 3) << "a third of the programs call between compiled functions: the leaves a driver calls make no calls of their own";
  std::printf("  generated call graphs: %llu programs, %llu compiled calls, %llu call exits\n", (unsigned long long)compared, (unsigned long long)calls,
      (unsigned long long)call_exit_total);
}

// ---------------------------------------------------------------------------
// Teardown, in every order the API allows
// ---------------------------------------------------------------------------

TEST(JitCalls, AnExecutionDestroyedWithCompiledCodeAndAPausedChainLeaksNothingInEitherOrder) {
  GLTANG_REQUIRE_JIT_BACKEND();
  const char * source =
      "function down(n) { if (n == 0) { i = 0; while (i < 5000) { i = i + 1; } return i; } return down(n - 1) + 1; }\n"
      "print(down(40));\n";
  for (int order = 0; order < 3; ++order) {
    Compiled compiled(source, Mode::Script, "teardown.tang");
    ASSERT_TRUE(compiled.ok());
    Config config;
    config.fuel = 1200;
    config.jit_threshold = 1;
    Context context(compiled.program, config);
    ASSERT_TRUE(context.ok());
    ASSERT_FALSE(context.execute());
    ASSERT_TRUE(context.paused());
    EXPECT_GT(context.jit_stats().functions_compiled, 0u);
    EXPECT_GE(context.jit_stats().refused_pauses, 1u) << "the pause was taken in compiled code, so the chain was rebuilt and is paused";
    if (order == 1) {
      gltang_execution_destroy(context.execution);  // the execution first, then the context
    }
    if (order == 2) {
      // Resumed to the end, then destroyed with its compiled code still registered.
      EXPECT_TRUE(context.finished_after_raising(1000000));
      EXPECT_EQ(context.raw(), "5040");
    }
    // The Context's destructor destroys the context (and the execution with it
    // when it was not destroyed above); the tracker checks every block and page.
  }
}

}  // namespace

#else  // GLTANG_WITH_JIT

TEST(NoJitCalls, TheCallStatisticsAreZeroAndNothingIsCompiled) {
  Compiled page("function f(n) { if (n < 2) { return n; } return f(n - 1) + f(n - 2); } print(f(12));", Mode::Script, "nojit.tang");
  ASSERT_TRUE(page.ok());
  Config config;
  Context context(page.program, config);
  ASSERT_TRUE(context.ok());
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(context.raw(), "144");
  GLTANG_JitStats stats;
  std::memset(&stats, 0xAB, sizeof stats);
  ASSERT_EQ(gltang_execution_jit_stats(context.execution, &stats), GLTANG_OK);
  EXPECT_EQ(stats.calls + stats.call_exits_remembered + stats.call_exits_push_refused + stats.call_exits_callee_guard + stats.call_exits_native_stack +
                stats.compile_at_call + stats.compile_hook_calls + stats.deepest_chain + stats.hook_argument_errors + stats.rebuild_failures +
                stats.last_exit_cause,
      0u);
}

#endif  // GLTANG_WITH_JIT

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  tt::install_watchdog(240);
  return RUN_ALL_TESTS();
}
