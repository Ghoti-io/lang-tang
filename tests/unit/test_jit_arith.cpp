/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2024-2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Lang-tang.
 *
 * Ghoti.io Lang-tang is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License version
 * 3 as published by the Free Software Foundation.
 *
 * Ghoti.io Lang-tang is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// Float and division operations compiled (spec-runtime-float story 7, CAP-5).
//
// `+ - * /`, unary minus, the six comparisons and `as int` / `as float` over a raw
// double, a small integer or a float read from a value word, and integer `/` and
// `%`, run in compiled code with no allocation, and every case the interpreter turns
// into an error or a heap allocation leaves compiled code before the operation's
// effect. The tests say that in numbers: a function that computes and returns a
// word (a counter) has no exit of its own, so no exit beyond the top level's is the proof the operations
// were compiled; the heap's allocation count over two loop lengths has slope 0; and
// where an exit is the point, the boxes the rebuild makes name the instruction it
// was made at. Each can fail, and has been seen to: tests/planted/48 to 59.
//
// A function that exits eight times is let go, so a test that exits runs each
// exit in a function (or an execution) of its own. The top level is compiled too, and
// leaves compiled code once, at its first function declaration: that exit is in every
// count below as `kMain`, and what a test asserts is the exits beyond it.

#include "exec_harness.h"
#include "jit_harness.h"
#include "test_helpers.h"
#include <ghoti.io/lang-tang/bytecode.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace jt;

#ifdef GLTANG_WITH_JIT

namespace {

/// The values every table is run over, as source: both zeros, infinities, NaN, a subnormal, the
/// largest double, ordinary floats, integers (including 2^53 + 1, which a double cannot hold, and both
/// bounds of the small-integer range).
const char * const kPrelude =
    "big = \"1.7976931348623157e308\" as float; pinf = big * 10.0; ninf = 0.0 - pinf; nan = pinf - pinf; nz = -0.0;\n"
    "tiny = \"5e-324\" as float; small = \"2.2250738585072014e-308\" as float; smax = 576460752303423487; smin = (0 - 576460752303423487) - 1;\n";

const char * const kValues = "[0.0, nz, 1.5, -2.5, pinf, ninf, nan, tiny, big, 3, -3, 9007199254740993, smax, smin, small]";
const char * const kNonZero = "[1.5, -2.5, pinf, ninf, nan, tiny, big, 3, -3, 9007199254740993, smax, smin, small]";

/// The exit of the top level (it leaves compiled code at its first declaration), which every program counts.
constexpr uint64_t kMain = 1;

/// What a function that reads the specials declares (a function sees a global only by naming it).
const char * const kGlobals = "global big; global pinf; global ninf; global nan; global nz; global tiny; global small; global smax; global smin; ";

/// `function f(a) { ... }` with the declarations of the globals first in its body.
std::string with_globals(std::string function) {
  size_t at = function.find("{ ");
  function.insert(at + 2, kGlobals);
  return function;
}

/// A scenario for one program.
Scenario scenario(const std::string & source) {
  Scenario sc;
  sc.source = source;
  return sc;
}

}  // namespace

// ---------------------------------------------------------------------------
// The allocation slope
// ---------------------------------------------------------------------------

TEST(JitArith, ALoopOfPlusMinusAndTimesOverARawFloatAndASmallIntegerAllocatesNothingPerIteration) {
  GLTANG_REQUIRE_JIT_BACKEND();
  const char * bodies[] = {
      "s = s + i * 0.5;",
      "s = s - i * 0.25;",
      "s = (s + 1.5) * 0.999;",
      "s = s + i;",
  };
  for (const char * body : bodies) {
    auto with = [&](const char * n) {
      return scenario(std::string("function f(n) { s = 0.0; i = 0; while (i < n) { ") + body + " i = i + 1; } print(s); }\nf(" + n + ");\n");
    };
    Outcome plain_short, jit_short, plain_long, jit_long;
    expect_same(with("200"), &plain_short, &jit_short);
    expect_same(with("5000"), &plain_long, &jit_long);
    ASSERT_FALSE(HasFailure()) << body;
    EXPECT_GE(jit_long.stats.entries, 1u) << body;
    EXPECT_EQ(jit_long.stats.compile_failures, 0u) << body;
    EXPECT_EQ(jit_long.heap_allocations, jit_short.heap_allocations) << body << ": the compiled loop allocates nothing per iteration";
    EXPECT_GT(plain_long.heap_allocations, plain_short.heap_allocations + 4000u) << body << ": the interpreter allocates a result per operation";
    EXPECT_EQ(jit_short.stats.deopts, jit_long.stats.deopts) << body << ": the exits do not grow with the loop";
    EXPECT_EQ(jit_long.stats.deopts, kMain + 1u) << body << ": the one exit of the function is the print";
    EXPECT_EQ(jit_long.stats.values_boxed, jit_short.stats.values_boxed) << body;
    EXPECT_EQ(jit_long.stats.conversion_errors, 0u) << body;
    std::printf("  arithmetic slope (%s): compiled allocations %llu at 200 and %llu at 5000; interpreted %llu and %llu\n", body,
        static_cast<unsigned long long>(jit_short.heap_allocations), static_cast<unsigned long long>(jit_long.heap_allocations),
        static_cast<unsigned long long>(plain_short.heap_allocations), static_cast<unsigned long long>(plain_long.heap_allocations));
  }
}

TEST(JitArith, TheSlopeIsStillZeroWithAHandlerPendingAtEveryPollAndUnderTortureAndVerify) {
  GLTANG_REQUIRE_JIT_BACKEND();
  for (int arm = 0; arm < 2; ++arm) {
    uint64_t allocations[2] = {0, 0}, boxed[2] = {0, 0}, deopts[2] = {0, 0};
    const char * lengths[2] = {arm == 0 ? "100" : "60", arm == 0 ? "2000" : "360"};
    for (int which = 0; which < 2; ++which) {
      Scenario sc = scenario(std::string("function f(n) { s = 0.0; i = 0; while (i < n) { s = s + i * 0.5 - 0.25; i = i + 1; } print(s); }\nf(") + lengths[which] + ");\n");
      sc.script = true;  // a request stays pending, so every poll takes the slow path
      if (arm == 1) {
        sc.torture = 1;
        sc.verify = 1;
      }
      Outcome plain, jit;
      expect_same(sc, &plain, &jit);
      ASSERT_FALSE(HasFailure());
      allocations[which] = jit.heap_allocations;
      boxed[which] = jit.stats.values_boxed;
      deopts[which] = jit.stats.deopts;
      EXPECT_EQ(jit.heap_invalid_pointers, 0u) << "the collector never read a raw word as a value";
      EXPECT_EQ(jit.stats.conversion_errors, 0u);
    }
    EXPECT_EQ(allocations[0], allocations[1]) << (arm == 0 ? "a handler pending" : "torture and verify") << ": a slow poll that continues allocates nothing";
    EXPECT_EQ(boxed[0], boxed[1]);
    EXPECT_EQ(deopts[0], deopts[1]);
  }
}

// ---------------------------------------------------------------------------
// Every operation is compiled: a function that computes and returns a word never exits
// ---------------------------------------------------------------------------

namespace {

struct OpCase {
  const char * name;
  const char * body;   ///< Uses `x` (a raw float, `a as float`), `a` (the value word) and `c` (a counter it raises).
  bool no_zero;        ///< The values include no zero (the body divides by one of them).
};

/// Runs `function f(a) { x = a as float; c = 0; <body> return c; }` over the values; with no exit
/// the whole run is compiled.
void run_compiled_op(const OpCase & op, const char * values, uint64_t expect_deopts) {
  Scenario sc = scenario(std::string(kPrelude) + "function f(a) { x = a as float; c = 0; " + op.body + " return c; }\n" +
      "vals = " + values + "; t = 0; k = 0; while (k < 13) { t = t + f(vals[k]); k += 1; } print(t);\n");
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  ASSERT_FALSE(::testing::Test::HasFailure()) << op.name;
  EXPECT_GE(jit.stats.entries, 1u) << op.name;
  EXPECT_EQ(jit.stats.compile_failures, 0u) << op.name << ": the verifier accepted every guard";
  EXPECT_EQ(jit.stats.deopts, kMain + expect_deopts) << op.name << ": " << (expect_deopts == 0 ? "compiled, with no exit" : "exits only where the interpreter must answer");
  EXPECT_EQ(jit.stats.conversion_errors, 0u) << op.name;
  EXPECT_EQ(jit.stats.rebuild_failures, 0u) << op.name;
  EXPECT_EQ(jit.errors, plain.errors) << op.name;
}

}  // namespace

TEST(JitArith, EveryOperationOverARawOperandRunsInCompiledCodeWithNoExit) {
  GLTANG_REQUIRE_JIT_BACKEND();
  const OpCase ops[] = {
      {"add, raw and word", "y = x + a; if (y == y) { c = c + 1; } if (y != y) { c = c + 2; }", false},
      {"add, word and raw", "y = a + x; if (y == y) { c = c + 1; }", false},
      {"add, raw and raw", "y = x + x; if (y >= 0.0) { c = c + 1; }", false},
      {"add, raw and a literal", "y = x + 1.5; if (y < 100.0) { c = c + 1; }", false},
      {"add, a small integer literal", "y = x + 3; if (y > 2.0) { c = c + 1; }", false},
      {"sub, raw and word", "y = x - a; if (y <= 1.0) { c = c + 1; }", false},
      {"sub, word and raw", "y = a - x; if (y <= 1.0) { c = c + 1; }", false},
      {"sub, an integer first", "y = 10 - x; if (y > 0.0) { c = c + 1; }", false},
      {"mul, raw and word", "y = x * a; if (y == 0.0) { c = c + 1; }", false},
      {"mul, word and raw", "y = a * x; if (y != 0.0) { c = c + 1; }", false},
      {"mul, an integer", "y = x * 7; if (y < 0.0) { c = c + 1; }", false},
      {"div, raw and word", "y = x / a; if (y == y) { c = c + 1; }", true},
      {"div, word and raw", "y = a / x; if (y == y) { c = c + 1; }", true},
      {"div, an integer by a raw", "y = 7 / x; if (y > 0.0) { c = c + 1; }", true},
      {"div, a raw by an integer", "y = x / 2; if (y > 0.0) { c = c + 1; }", false},
      {"neg", "y = -x; if (y < 0.0) { c = c + 1; } if (y == x) { c = c + 2; }", false},
      {"neg of a negation", "y = -(-x); if (y == x) { c = c + 1; }", false},
      {"lt", "if (x < a) { c = c + 1; } if (a < x) { c = c + 2; } if (x < 1.5) { c = c + 4; }", false},
      {"le", "if (x <= a) { c = c + 1; } if (a <= x) { c = c + 2; } if (x <= 3) { c = c + 4; }", false},
      {"gt", "if (x > a) { c = c + 1; } if (a > x) { c = c + 2; } if (x > 3) { c = c + 4; }", false},
      {"ge", "if (x >= a) { c = c + 1; } if (a >= x) { c = c + 2; } if (x >= 1.5) { c = c + 4; }", false},
      {"eq", "if (x == a) { c = c + 1; } if (a == x) { c = c + 2; } if (x == x) { c = c + 4; }", false},
      {"ne", "if (x != a) { c = c + 1; } if (a != x) { c = c + 2; } if (x != x) { c = c + 4; }", false},
      {"as float", "y = x as float; z = a as float; if (y == z) { c = c + 1; }", false},
      {"as int of a raw operand in range", "y = (0.5 * 15.0) as int; z = y + 1; if (z == 8) { c = c + 1; }", false},
  };
  // Values on which no operation above leaves compiled code: no zero divisor. The `a as float` of a value
  // is the same operand either way, so one list serves; a value of the other list would only exit.
  for (const OpCase & op : ops) {
    run_compiled_op(op, op.no_zero ? kNonZero : kValues, 0);
    if (HasFailure()) {
      return;
    }
  }
}

TEST(JitArith, EveryComparisonOfANaNIsFalseExceptNotEqualInCompiledCodeAsInTheInterpreter) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc = scenario(std::string(kPrelude) +
      "function f(n) { " + kGlobals + "x = nan; y = 1.5; c = 0; i = 0; while (i < n) {\n"
      "  if (x < y) { c = c + 1; } if (x <= y) { c = c + 2; } if (x > y) { c = c + 4; } if (x >= y) { c = c + 8; } if (x == y) { c = c + 16; } if (x != y) { c = c + 32; }\n"
      "  if (y < x) { c = c + 64; } if (y <= x) { c = c + 128; } if (y > x) { c = c + 256; } if (y >= x) { c = c + 512; } if (y == x) { c = c + 1024; } if (y != x) { c = c + 2048; }\n"
      "  if (x == x) { c = c + 4096; } if (x != x) { c = c + 8192; } if (0.0 == nz) { c = c + 16384; } if (2 == 2.0) { c = c + 32768; }\n"
      "  if (9007199254740993 == 9007199254740992.0) { c = c + 65536; }\n"
      "  i = i + 1; } return c; }\nprint(f(3));\n");
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  ASSERT_FALSE(HasFailure());
  // The NaN in a local is `nan`, a global read: LOAD_GLOBAL is a word, so these compare a word and a raw.
  EXPECT_EQ(jit.stats.compile_failures, 0u);
  EXPECT_EQ(jit.raw, plain.raw);
}

TEST(JitArith, ANaNHeldRawComparesAsTheInterpreterDoes) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // nan is made raw in the function (a cast of a word), so both sides of each comparison are raw and the
  // float compare itself runs: false for all four orders and for ==, true for !=.
  Scenario sc = scenario(std::string(kPrelude) +
      "function f(v) { x = v as float; y = 1.5; c = 0;\n"
      "  if (x < y) { c = c + 1; } if (x <= y) { c = c + 2; } if (x > y) { c = c + 4; } if (x >= y) { c = c + 8; } if (x == y) { c = c + 16; } if (x != y) { c = c + 32; }\n"
      "  if (y < x) { c = c + 64; } if (y <= x) { c = c + 128; } if (y > x) { c = c + 256; } if (y >= x) { c = c + 512; } if (y == x) { c = c + 1024; } if (y != x) { c = c + 2048; }\n"
      "  if (x == x) { c = c + 4096; } if (x != x) { c = c + 8192; }\n"
      "  return c; }\nprint(f(nan)); print(f(1.5)); print(f(2)); print(f(pinf)); print(f(ninf));\n");
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(jit.stats.deopts, kMain) << "nothing left compiled code";
  EXPECT_NE(jit.raw.find("10272"), std::string::npos) << "f(nan): != x2 (32 + 2048) and x != x (8192): " << jit.raw;
}

// ---------------------------------------------------------------------------
// The matrix rows with a number in them
// ---------------------------------------------------------------------------

TEST(JitArith, MixedIntegerAndFloatConvertTheIntegerAndKeepTheOrderOfTheOperands) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc = scenario(
      "function f(a) { b = a * 1.5; c = 1.5 - a; d = a / 2.0; e = 7 / 2.0; g = 2.0 / a; h = 1.5 - a * 1.0; print(b); print(c); print(d); print(e); print(g); print(h); return 0; }\n"
      "f(3); f(-4); f(9007199254740993); f(576460752303423487);\n");
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(jit.stats.compile_failures, 0u);
  // The first print is the only exit of each call; what it boxes is what was live: b, c, d, e, g, h and the
  // operand slot. An exit earlier (at an operation) would have boxed fewer.
  EXPECT_EQ(jit.stats.values_boxed, 4u * 7u) << "each call exited at its first print, with six locals and the operand raw";
  EXPECT_EQ(jit.stats.deopts, kMain + 4u);
  EXPECT_NE(jit.raw.find("4.5-1.51.53.50.666667-1.5"), std::string::npos) << jit.raw;
}

TEST(JitArith, AFloatReadFromAValueWordIsUnboxedAnAnIntegerIsConvertedAndAnythingElseExitsBeforeAnyEffect) {
  GLTANG_REQUIRE_JIT_BACKEND();
  struct Case {
    const char * argument;
    bool compiled;  ///< Stays in compiled code to the print.
    const char * raw;
  };
  const Case cases[] = {
      {"1.5", true, "6."}, {"2", true, "8."}, {"-3", true, "-12."},   {"\"ab\"", false, "0.0ababab"}, {"null", false, ""}, {"true", false, ""},
      {"[1]", false, ""}, {"(smax + 1)", false, ""},
  };
  for (const Case & c : cases) {
    Scenario sc = scenario(std::string(kPrelude) + "function f(x) { s = 0.0; i = 0; while (i < 4) { s = s + x; i = i + 1; } print(s); return 0; }\nf(" + c.argument + ");\n");
    Outcome plain, jit;
    expect_same(sc, &plain, &jit);
    ASSERT_FALSE(HasFailure()) << c.argument;
    EXPECT_EQ(jit.stats.deopts, kMain + 1u) << c.argument << ": one exit, at the print or at the first operation that could not be done";
    EXPECT_EQ(jit.stats.compile_failures, 0u) << c.argument;
    if (c.compiled) {
      EXPECT_EQ(jit.stats.values_boxed, 2u) << c.argument << ": the exit was the print, with s and its operand raw";
      EXPECT_EQ(jit.raw.compare(0, std::strlen(c.raw), c.raw), 0) << c.argument << ": " << jit.raw;
    }
    else {
      EXPECT_FALSE(plain.errors.empty() && std::string(c.argument) == "null") << "null cannot be added to a float: an error";
    }
  }
}

TEST(JitArith, AGuardThatFailsInTheMiddleOfAnOperationWritesNothingAndTheRebuildBoxesEachLiveValueOnce) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // acc is raw; the string operand fails the unbox guard at the add of the first iteration. No slot has been
  // written: the frame the interpreter resumes has acc, in its local and as the first operand, each boxed once.
  Scenario sc = scenario("function f(x) { acc = 0.5; i = 0; while (i < 3) { acc = acc + x; i = i + 1; } print(acc); return i; }\nf(\"s\");\n");
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(jit.stats.deopts, kMain + 1u);
  EXPECT_EQ(jit.stats.values_boxed, 2u) << "acc as a local and acc as the operand, once each";
  EXPECT_EQ(jit.raw, "0.5sss");
  // The same on the third call of a function that had been adding floats: the first two calls run to the end.
  Scenario third = scenario("function f(x) { acc = 0.5; i = 0; while (i < 3) { acc = acc + x; i = i + 1; } return i; }\n"
                            "print(f(1.5)); print(f(2)); print(f(\"s\")); print(f(2.5));\n");
  Outcome p3, j3;
  expect_same(third, &p3, &j3);
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(j3.stats.deopts, kMain + 1u) << "only the call with the string left compiled code";
  EXPECT_EQ(j3.stats.values_boxed, 2u);
}

// ---------------------------------------------------------------------------
// The error cases: compiled code exits at the instruction, the interpreter answers
// ---------------------------------------------------------------------------

TEST(JitArith, EveryCaseTheInterpreterTurnsIntoAnErrorExitsAtTheInstructionAndTheErrorListIsTheInterpreters) {
  GLTANG_REQUIRE_JIT_BACKEND();
  struct Case {
    const char * name;
    const char * function;   ///< Defines f(a), run with a float, which has no effect before the instruction that exits.
    bool errors;             ///< The interpreter reports an error.
  };
  const Case cases[] = {
      {"float divide by +0.0", "function f(a) { x = a as float; z = x / 0.0; return 0; }", true},
      {"float divide by -0.0", "function f(a) { x = a as float; z = x / nz; return 0; }", true},
      {"float divide by integer zero", "function f(a) { x = a as float; z = x / 0; return 0; }", true},
      {"0.0 / 0.0", "function f(a) { x = a as float; z = 0.0 / 0.0; return 0; }", true},
      {"a word divided by a raw zero", "function f(a) { x = a as float; y = x - x; z = a / y; return 0; }", true},
      {"integer divide by zero", "function f(a) { z = 7 / (a as int - a as int); return 0; }", true},
      {"integer modulo by zero", "function f(a) { z = 7 % (a as int - a as int); return 0; }", true},
      {"zero over zero", "function f(a) { z = 0 / (a as int - a as int); return 0; }", true},
      {"modulo of a raw operand", "function f(a) { x = a as float; z = x % 2; return 0; }", true},
      {"modulo by a raw operand", "function f(a) { x = a as float; z = 2 % x; return 0; }", true},
      {"modulo of a float word", "function f(a) { z = a % 2; return 0; }", true},
      {"SMALL_MIN / -1", "function f(a) { z = smin / (a as int - a as int - 1); return 0; }", false},
      {"SMALL_MAX + 1", "function f(a) { z = smax + (a as int - a as int + 1); return 0; }", false},
      {"SMALL_MAX * 2", "function f(a) { z = smax * (a as int - a as int + 2); return 0; }", false},
      {"as int of NaN", "function f(a) { x = a as float; z = (x - x + nan) as int; return 0; }", true},
      {"as int of an infinity", "function f(a) { x = a as float; z = (x + pinf) as int; return 0; }", true},
      {"as int of 2^59", "function f(a) { x = a as float; z = (x - x + 576460752303423488.0) as int; return 0; }", false},
      {"as int of 1e19", "function f(a) { x = a as float; z = (x - x + (\"1e19\" as float)) as int; return 0; }", true},
      {"a raw operand and a string", "function f(a) { x = a as float; z = x + \"s\"; return 0; }", false},
      {"a raw operand and null", "function f(a) { x = a as float; z = x * null; return 0; }", true},
      {"a raw operand and an array", "function f(a) { x = a as float; z = x - [1]; return 0; }", true},
      {"a raw operand and a boxed integer", "function f(a) { x = a as float; z = x + (smax + 1); return 0; }", false},
      {"a comparison with a string", "function f(a) { x = a as float; z = x < \"a\"; return 0; }", true},
      {"an equality with null", "function f(a) { x = a as float; z = x == null; return 0; }", false},
      {"an equality with a string", "function f(a) { x = a as float; z = x != \"a\"; return 0; }", false},
      {"as float of a string", "function f(a) { z = \"x\" as float; return 0; }", true},
  };
  for (const Case & c : cases) {
    // The result is printed, so that an error value is entered in the error list.
    std::string function = with_globals(c.function);
    function.insert(function.rfind("return"), "print(z); ");
    Scenario sc = scenario(std::string(kPrelude) + function + "\nf(1.5); f(2);\nprint(\"done\");\n");
    Outcome plain, jit;
    expect_same(sc, &plain, &jit);
    ASSERT_FALSE(HasFailure()) << c.name;
    EXPECT_EQ(jit.stats.compile_failures, 0u) << c.name;
    EXPECT_EQ(jit.stats.rebuild_failures, 0u) << c.name;
    EXPECT_GT(jit.stats.deopts, kMain) << c.name << ": the instruction is a guard exit";
    EXPECT_EQ(plain.raw.find('[') != std::string::npos || !plain.errors.empty(), c.errors)
        << c.name << ": the interpreter " << (c.errors ? "makes" : "does not make") << " an error value (it prints in brackets or is entered in the list): " << plain.raw << " / " << plain.errors;
    EXPECT_EQ(jit.raw, plain.raw) << c.name;
  }
}

TEST(JitArith, ADivisionByNaNOrInfinityIsNotAZeroDivisorAndIsCompiled) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc = scenario(std::string(kPrelude) +
      "function f(v) { " + std::string(kGlobals) + "a = v as float; b = a / nan; c = (pinf as float) / pinf; d = 1.5 / pinf; e = a / ninf; g = nan / a; h = 0.0 / a;\n"
      "  k = 0; if (b != b) { k = k + 1; } if (c != c) { k = k + 2; } if (d == 0.0) { k = k + 4; } if (e == 0.0) { k = k + 8; } if (g != g) { k = k + 16; } if (h == 0.0) { k = k + 32; } return k; }\n"
      "print(f(1.5)); print(f(-2)); print(f(3));\n");
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(jit.stats.deopts, kMain);
  EXPECT_EQ(jit.raw, "636363");
}

TEST(JitArith, IntegerDivisionAndModuloOfSmallIntegersAreCompiledAndTheRangeAndZeroCasesExit) {
  GLTANG_REQUIRE_JIT_BACKEND();
  struct Case {
    const char * a;
    const char * b;
    bool exits;
  };
  const Case cases[] = {
      {"7", "2", false},     {"-7", "2", false},     {"7", "-2", false},        {"-7", "-2", false},        {"0", "5", false},     {"smax", "1", false},
      {"smin", "1", false},  {"smax", "-1", false},  {"smin", "2", false},      {"smax", "smax", false},    {"1", "smin", false},  {"5", "-1", false},
      {"smin", "-1", true},  {"7", "0", true},       {"0", "0", true},          {"smin", "0", true},
  };
  for (const Case & c : cases) {
    // Two functions, so that the quotient's exit and the remainder's do not end one call before the other: a
    // function returns its word (no print: a print is itself an exit), and the top level prints the two.
    Scenario sc = scenario(std::string(kPrelude) + "function f(a, b) { q = a / b; return q; }\nfunction g(a, b) { r = a % b; return r; }\n" +
        "print(f(" + c.a + ", " + c.b + ")); print(\" \"); print(g(" + c.a + ", " + c.b + ")); print(f(" + c.a + ", " + c.b + ")); print(g(" + c.a + ", " + c.b + "));\n");
    Outcome plain, jit;
    expect_same(sc, &plain, &jit);
    ASSERT_FALSE(HasFailure()) << c.a << " / " << c.b;
    EXPECT_EQ(jit.stats.compile_failures, 0u) << c.a << " / " << c.b << ": the verifier accepted the guards";
    EXPECT_EQ(jit.stats.deopts > kMain, c.exits) << c.a << ", " << c.b << ": " << (c.exits ? "an exit" : "compiled to the end");
  }
  // The values themselves, where the matrix names them.
  Scenario sc = scenario("function f(a, b) { return a / b; }\nfunction g(a, b) { return a % b; }\n"
                         "print(f(7, 2)); print(\" \"); print(f(-7, 2)); print(\" \"); print(f(7, -2)); print(\" \"); print(g(-7, 2)); print(\" \"); print(g(7, -2)); print(\" \"); print(g(5, -1)); print(\" \"); print(f(0, 5));\n");
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(jit.raw, "3 -3 -3 -1 1 0 0");
  EXPECT_EQ(jit.stats.deopts, kMain);
}

TEST(JitArith, AnIntegerResultOutsideTheSmallIntegerRangeExitsAndTheInterpreterMakesTheBoxedInteger) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc = scenario(std::string(kPrelude) +
      "function f(a) { x = smin / (a - 2); y = x + 1; return y; }\nfunction g(a) { x = smax + a; y = x - a; return y; }\n"
      "print(f(1)); print(\" \"); print(g(1)); print(\" \"); print(g(2));\n");
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  ASSERT_FALSE(HasFailure());
  EXPECT_GT(jit.stats.deopts, kMain);
  EXPECT_EQ(jit.stats.compile_failures, 0u);
}

TEST(JitArith, ACastToIntOfADoubleInsideTheSmallRangeIsCompiledAndEverythingElseExits) {
  GLTANG_REQUIRE_JIT_BACKEND();
  struct Case {
    const char * value;
    bool exits;
    bool exits_from_a_word = false;   ///< The other shape: `a as int` of the word, where 2^59 - 1 is a small integer already.
    bool differs = false;
  };
  const Case cases[] = {
      {"1.9", false},
      {"-1.9", false},
      {"-0.5", false},
      {"0.0", false},
      {"nz", false},
      {"576460752303423424.0", false},        // 2^59 - 64: the largest double below 2^59
      {"-576460752303423488.0", false},       // -2^59: the smallest small integer
      {"576460752303423488.0", true},         // 2^59: the interpreter makes a boxed integer
      {"-576460752303423616.0", true},        // -2^59 - 128
      {"(\"1e18\" as float)", true},
      {"(\"1e19\" as float)", true},
      {"pinf", true},
      {"ninf", true},
      {"nan", true},
      {"5", false},
      {"smax", true, false, true},     // (double)(2^59 - 1) rounds to 2^59: out of range as a double, a small integer as a word
      {"smin", false},
  };
  for (const Case & c : cases) {
    // From a raw operand, and from a value word (the float read from the word).
    for (int shape = 0; shape < 2; ++shape) {
      const char * fn = shape == 0 ? "function f(a) { x = a as float; y = x as int; return y; }" : "function f(a) { y = a as int; return y; }";
      Scenario sc = scenario(std::string(kPrelude) + fn + "\nprint(f(" + c.value + ")); print(\" \"); print(f(" + c.value + "));\n");
      Outcome plain, jit;
      expect_same(sc, &plain, &jit);
      ASSERT_FALSE(HasFailure()) << c.value << " shape " << shape;
      EXPECT_EQ(jit.stats.compile_failures, 0u) << c.value;
      const bool exits = shape == 1 && c.differs ? c.exits_from_a_word : c.exits;
      EXPECT_EQ(jit.stats.deopts > kMain, exits) << c.value << " shape " << shape << ": " << (c.exits ? "an exit" : "compiled to the end");
    }
  }
}

TEST(JitArith, ACastToIntTruncatesTowardZeroAndACastToFloatKeepsTheBits) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc = scenario(std::string(kPrelude) +
      "function f(a) { x = a as float; y = x as int; return y; }\nfunction g(a) { x = a as float; y = x as float; z = y - 0.5; w = z + 0.5; return w == x; }\n"
      "print(f(1.9)); print(\" \"); print(f(-1.9)); print(\" \"); print(f(-0.5)); print(\" \"); print(f(7)); print(\" \"); print(g(1.5)); print(g(3)); print(g(9007199254740993)); print(g(nan));\n");
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(jit.raw, "1 -1 0 7 truetruetruefalse");
  EXPECT_EQ(jit.stats.deopts, kMain);
}

TEST(JitArith, UnaryMinusOfARawFloatFlipsTheSignBitAloneSoNegativeZeroAndNaNAreRight) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // -0.0 for 0.0 (text "-0."), 0. for -0.0, a NaN stays a NaN, and an integer's minus is the integer path.
  Scenario sc = scenario(std::string(kPrelude) +
      "function f(a) { x = a as float; y = -x; z = -y; w = -a; print(y); print(\" \"); print(z); print(\" \"); print(y == x); return 0; }\n"
      "function g(a) { x = a as float; y = -x; if (y != y) { return 1; } if (y > 0.0) { return 2; } if (y < 0.0) { return 3; } return 4; }\n"
      "f(0.0); print(\" | \"); f(nz); print(\" | \"); f(2.5); print(\" | \"); f(nan); print(\" | \"); f(-7);\n"
      "print(g(0.0)); print(g(nz)); print(g(nan)); print(g(pinf)); print(g(-3)); print(g(4));\n");
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  ASSERT_FALSE(HasFailure());
  EXPECT_NE(jit.raw.find("-0. 0. true"), std::string::npos) << "-(0.0) is negative zero: " << jit.raw;
  EXPECT_NE(jit.raw.find("0. -0. true"), std::string::npos) << "-(-0.0) is positive zero: " << jit.raw;
  EXPECT_NE(jit.raw.find("nan"), std::string::npos) << jit.raw;
  EXPECT_NE(jit.stats.entries, 0u);
}

// ---------------------------------------------------------------------------
// Consumers of a computed float
// ---------------------------------------------------------------------------

TEST(JitArith, AComputedFloatThatReachesAConsumerThatIsNotAnOperationExitsThereBoxedOnce) {
  GLTANG_REQUIRE_JIT_BACKEND();
  const char * consumers[] = {
      "print(s);", "return s;", "y = h(s, 1); print(y);", "g = s; print(g);", "if (s) { i = i + 1; } print(i);", "y = s && 1; print(y);", "y = s || 1; print(y);",
      "y = !s; print(y);", "y = s as string; print(y);", "y = s as bool; print(y);", "y = s % 2; print(y);", "y = [s]; print(y[0]);", "y = s.name; print(y);",
  };
  for (const char * consumer : consumers) {
    Scenario sc = scenario(std::string("g = 0;\nfunction h(a, b) { return a; }\n") +
        "function f(n) { global g; s = 0.0; i = 0; while (i < n) { s = s + i * 0.5; i = i + 1; } " + consumer + " return i; }\nprint(f(5)); print(f(0));\n");
    Outcome plain, jit;
    expect_same(sc, &plain, &jit);
    EXPECT_GT(jit.stats.deopts, kMain) << consumer << ": the consumer is an unconditional exit";
    EXPECT_GE(jit.stats.values_boxed, 1u) << consumer << ": the operand was boxed by the rebuild";
    EXPECT_EQ(jit.stats.compile_failures, 0u) << consumer;
    EXPECT_EQ(jit.stats.conversion_errors, 0u) << consumer;
    EXPECT_EQ(jit.stats.rebuild_failures, 0u) << consumer;
    if (HasFailure()) {
      ADD_FAILURE() << "with the consumer: " << consumer;
      return;
    }
  }
}

TEST(JitArith, AMergeOfUnequalKindsStillCompilesWithNoRawSlot) {
  GLTANG_REQUIRE_JIT_BACKEND();
  const char * programs[] = {
      "function f(c) { x = 0; i = 0; while (i < 4) { x = x + 0.5; i += 1; } print(x); return c; }\nf(1); f(0);\n",
      "function f(c, a) { if (c) { x = a * 1.5; } else { x = a; } print(x); return 0; }\nf(true, 3); f(false, 3); f(true, 2.5);\n",
  };
  for (const char * source : programs) {
    Outcome plain, jit;
    expect_same(scenario(source), &plain, &jit);
    EXPECT_EQ(jit.stats.compile_failures, 0u) << source;
    EXPECT_GE(jit.stats.entries, 1u) << source;
    EXPECT_EQ(jit.stats.values_boxed, 0u) << source << ": no raw slot, so no rebuild boxed anything";
  }
}

// ---------------------------------------------------------------------------
// Fuel, polls, torture and relocation
// ---------------------------------------------------------------------------

TEST(JitArith, FuelAndPollsAreTheInterpretersUnderATightCapAndAPauseEveryFewUnits) {
  GLTANG_REQUIRE_JIT_BACKEND();
  const std::string program = std::string(kPrelude) +
      "function f(n) { s = 0.0; a = n as float; i = 0; while (i < n) { s = s + a / (i + 1.5); a = a - 1.0; t = s as int; u = -s; i = i + 1; } print(s > 0.0); return i; }\n"
      "print(f(40)); print(f(3));\n";
  for (uint64_t cap : {uint64_t{150}, uint64_t{700}, uint64_t{GRCORE_UNLIMITED}}) {
    for (uint64_t step : {uint64_t{0}, uint64_t{37}}) {
      Scenario sc = scenario(program);
      sc.fuel = cap;
      sc.step = step;
      Outcome plain, jit;
      expect_same(sc, &plain, &jit);
      ASSERT_FALSE(HasFailure()) << "cap " << cap << " step " << step;
      EXPECT_EQ(jit.fuel, plain.fuel);
      EXPECT_EQ(jit.pauses, plain.pauses) << "the same poll for each pause";
    }
  }
  // Compiled code that ran, with the cap far away: the fuel of every operation is in the total.
  Scenario sc = scenario(program);
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  EXPECT_GE(jit.stats.entries, 1u);
  EXPECT_GT(jit.fuel, 500u);
}

TEST(JitArith, AFloatParameterLiveAcrossPollsIsReadThroughTheMovedObjectUnderTortureVerifyAndAHandler) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc = scenario(
      "function f(x, n) { s = 0.0; i = 0; while (i < n) { s = s + x * 2; t = x as int; i += 1; } print(s); return t; }\n"
      "function g(x) { a = x; b = [1.5, 2.5]; r = f(a, 12); return r; }\n"
      "print(g(1.75)); print(g(3)); print(g(-0.5));\n");
  sc.torture = 1;
  sc.verify = 1;
  sc.script = true;
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(jit.heap_invalid_pointers, 0u);
  EXPECT_EQ(jit.stats.conversion_errors, 0u);
  EXPECT_GE(jit.stats.entries, 1u);
  // And with the stack moving at every push.
  sc.moving = 1;
  Outcome plain2, jit2;
  expect_same(sc, &plain2, &jit2);
}

TEST(JitArith, ARebuildAtAnExitInTheMiddleOfAnOperationUnderTortureBoxesFromThePool) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc = scenario(
      "function f(x, y) { s = 0.25; i = 0; while (i < 6) { s = s * 1.5 + x; i += 1; } z = s + y; print(z); return i; }\n"
      "print(f(1.5, 2)); print(f(2, \"s\")); print(f(0.5, null)); print(f(1, [1]));\n");
  sc.torture = 1;
  sc.verify = 1;
  sc.script = true;
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(jit.heap_invalid_pointers, 0u);
  EXPECT_EQ(jit.stats.rebuild_failures, 0u);
}

TEST(JitArith, ACompiledCallBetweenFunctionsWithRawLocalsLiveBoxesBothFramesOnAnExit) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc = scenario(
      "function leaf(n, x) { t = 0.25; i = 0; while (i < n) { t = t * 1.5 + x; i += 1; } return n; }\n"
      "function mid(n, x) { u = 2.5; r = leaf(n, x); u = u + r; k = 0; while (k < 2) { k += 1; } v = u * 2.0; return k; }\n"
      "print(mid(3, 1.5)); print(mid(2, 2)); print(mid(4, \"s\"));\n");
  sc.script = true;
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  ASSERT_FALSE(HasFailure());
  EXPECT_GE(jit.stats.calls, 1u);
  EXPECT_EQ(jit.stats.rebuild_failures, 0u);
  EXPECT_EQ(jit.stats.conversion_errors, 0u);
}

TEST(JitArith, TheKindsOfAFunctionWithNoFloatAndNoCastAreWhatTheyWere) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc = scenario("function f(n) { s = 0; i = 0; while (i < n) { s = s + i * 2 - 1; i = i + 1; } return s; }\nprint(f(50));\n");
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(jit.stats.deopts, kMain);
  EXPECT_EQ(jit.stats.values_boxed, 0u);
  EXPECT_EQ(jit.stats.box_pool_made, 0u);
}

#else  // JIT=no

TEST(JitArith, WithoutTheJitTheSameProgramsRunInTheInterpreterAndNothingIsCompiled) {
  Compiled page("function f(n) { s = 0.0; i = 0; while (i < n) { s = s + i * 0.5; i = i + 1; } print(s); print(7 / 2); print(1.9 as int); return i; }\nprint(f(100));\n", Mode::Script, "nojit.tang");
  ASSERT_TRUE(page.ok());
  Context context(page.program);
  ASSERT_TRUE(context.ok());
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(context.raw(), "2475.31100");
  GLTANG_JitStats st = context.jit_stats();
  EXPECT_EQ(st.entries + st.deopts + st.functions_compiled + st.values_boxed, 0u);
}

#endif

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
