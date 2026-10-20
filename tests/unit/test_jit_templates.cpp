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

// The output operations in compiled code (spec-template-compiled story 1, CAP-1).
//
// `PRINT`, `PRINT_CONST` and `CLEAR_RESULT` run in compiled code as calls of the
// interpreter's own functions (`gltang_vm_op_print`, `gltang_vm_print_constant`,
// `gltang_vm_set_result`), so there is one implementation of printing. Each test runs
// one program interpreted and with every function compiled at its first poll, and
// requires the two runs to be the same run (output, result, error list, fuel, every
// poll and its identity; jit_harness.h), and then says in numbers that the prints ran
// compiled: the `output_calls` counter, and no exit beyond the top level's.
//
// The top level of a script leaves compiled code once, at its first function
// declaration (a global store is not compiled): that exit is `kMain` in every count
// below. A template's function 0 leaves at its `HALT`, which story 2 compiles.
//
// Each can fail, and has been seen to: tests/planted/152 to 156.

#include "exec_harness.h"
#include "jit_harness.h"
#include "test_helpers.h"

#include <ghoti.io/lang-tang/type.h>

#include <string>
#include <vector>

using namespace jt;

#ifdef GLTANG_WITH_JIT

namespace {

/// The exit of a script's top level, at its first declaration.
constexpr uint64_t kMain = 1;

Scenario scenario(const std::string & source, Mode mode = Mode::Script) {
  Scenario sc;
  sc.source = source;
  sc.mode = mode;
  sc.script = true;  // every poll takes the slow path and is counted, with its identity
  return sc;
}

/// What an output sink was given, and the call (from 0) it refuses; -1 for none.
struct Delivery {
  std::string bytes;
  int refuse_at = -1;
  int calls = 0;
};

bool sink_fn(void * user, const char * bytes, size_t length, GLTANG_String_Type) {
  Delivery * d = static_cast<Delivery *>(user);
  if (d->calls++ == d->refuse_at) {
    return false;
  }
  d->bytes.append(bytes, length);
  return true;
}

uint64_t native_polls(Context & context, const char * native) {
  uint64_t n = 0;
  EXPECT_EQ(gltang_execution_native_polls(context.execution, native, &n), GLTANG_OK);
  return n;
}

}  // namespace

TEST(JitTemplates, ALoopOfPrintsRunsCompiledWithNoExitAtAPrintAndTheSameBytesFuelAndPolls) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // `r = print(i)`: a print's value is null, so the function returns null; a print that
  // left its operand in place would return 99. The value is stored rather than dropped
  // because a bare `print(i);` in a function ends in `DISCARD` (and at the top level in
  // `SET_RESULT`), which this story does not compile: that statement still leaves compiled
  // code after its print, at the `DISCARD`.
  Scenario sc = scenario("function f(n) { r = 1; i = 0; while (i < n) { r = print(i); i = i + 1; } return r; }\nx = f(100);\nx;\n");
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  ASSERT_FALSE(HasFailure());
  std::string expected;
  for (int i = 0; i < 100; ++i) {
    expected += std::to_string(i);
  }
  EXPECT_EQ(jit.raw, expected);
  EXPECT_GT(jit.polls, 100u) << "the loop polled at every iteration, and the polls were compared";
  EXPECT_EQ(jit.stats.functions_compiled, 2u);
  EXPECT_EQ(jit.stats.output_calls, 100u) << "every print was a compiled call of the interpreter's print";
  EXPECT_EQ(jit.stats.deopts, kMain) << "no print left compiled code";
  EXPECT_EQ(jit.stats.functions_discarded, 0u);
  EXPECT_EQ(plain.stats.output_calls, 0u);
}

TEST(JitTemplates, ATemplateWhoseFirstOperationIsTextIsCompiledAndItsTextAndClearsRunCompiled) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // The function's first operation is `PRINT_CONST`, which the entry check used to
  // decline: the function was not compiled at all. Its one exit now is the `HALT`.
  Scenario sc = scenario("head<% if (1 < 2) { %>yes<% } else { %>no<% } %>tail", Mode::Template);
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(jit.raw, "headyestail");
  EXPECT_EQ(jit.stats.functions_compiled, 1u) << "the template's function 0 was compiled, not declined";
  EXPECT_EQ(jit.stats.entries, 1u);
  EXPECT_EQ(jit.stats.output_calls, 3u) << "the three texts were printed by compiled code";
  EXPECT_GE(jit.stats.result_clears, 1u) << "a text statement clears the template's result, in compiled code";
  EXPECT_EQ(jit.stats.deopts, 1u) << "the only exit is the HALT (story 2)";
}

TEST(JitTemplates, APrintOfAnErrorLogsItAsPrintedAndAppendsNothing) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // The error is made by the top level (1 / 0 is a guard exit in compiled code) and
  // printed by the compiled `f`.
  Scenario sc = scenario("function f(e) { r = print(e); print(\"|\"); return r; }\nx = f(1 / 0);\nx;\n");
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(jit.raw, "|") << "an error prints as nothing";
  EXPECT_NE(jit.errors.find(std::to_string((int)GLTANG_ERROR_HOW_PRINTED) + ":"), std::string::npos)
      << "the error was entered as printed: " << jit.errors;
  // The interpreter's print of an error swallows it, and the print's value is null (only a
  // print that fails, for memory or a refused delivery, has an error as its value).
  EXPECT_EQ(jit.result, plain.result);
  EXPECT_EQ(jit.result.find("Division by zero"), std::string::npos) << jit.result;
  EXPECT_EQ(jit.stats.output_calls, 2u);
  EXPECT_EQ(jit.stats.deopts, kMain);
}

TEST(JitTemplates, ALargePrintPollsAsOftenChargesTheSameFuelAndAScopeThatRunsOutInsideItStopsAtTheSamePoll) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // 20480 bytes: five pacing polls of GLTANG_POLL_BYTES each, inside the compiled call.
  {
    Scenario sc = scenario("function f(s) { r = print(s); print(\"|\"); return r; }\n"
                           "s = \"0123456789\"; for (k = 0; k < 11; k += 1) { s = s + s; }\nx = f(s);\nx;\n");
    std::vector<uint64_t> paced;
    sc.after = [&](Context & context) { paced.push_back(native_polls(context, "PRINT")); };
    Outcome plain, jit;
    expect_same(sc, &plain, &jit);
    ASSERT_FALSE(HasFailure());
    ASSERT_EQ(paced.size(), 2u);
    EXPECT_EQ(paced[0], paced[1]) << "the same pacing polls";
    EXPECT_GE(paced[1], 5u);
    EXPECT_EQ(jit.raw.size(), 20481u);
    EXPECT_EQ(jit.stats.output_calls, 2u);
    EXPECT_EQ(jit.stats.deopts, kMain) << "the large print polled inside compiled code and did not leave it";
  }
  // A template whose text is longer than the pacing interval, called with a scope of 60
  // fuel: the scope runs out at a pacing poll inside the compiled `PRINT_CONST`, the
  // helper reports the unwind, compiled code leaves through the chain deopt, and the
  // text printed so far is taken back.
  {
    Scenario sc = scenario("use big; print(\"[\"); print(big()); print(\"]\");\n");
    sc.parts.push_back(Part{"big", std::string(20000, 'x') + "<% k = 0; %>tail", 60, GLTANG_SCOPE_EMPTY, Mode::Template});
    std::vector<uint64_t> paced;
    sc.after = [&](Context & context) { paced.push_back(native_polls(context, "PRINT")); };
    Outcome plain, jit;
    expect_same(sc, &plain, &jit);
    ASSERT_FALSE(HasFailure());
    ASSERT_EQ(paced.size(), 2u);
    EXPECT_EQ(paced[0], paced[1]);
    EXPECT_GE(paced[1], 1u);
    EXPECT_TRUE(jit.raw == "[]") << "the scope ran out inside the text, which was rolled back: " << jit.raw.size() << " bytes";
    EXPECT_NE(jit.errors.find(std::to_string((int)GLTANG_ERROR_HOW_SCOPE_LIMIT) + ":"), std::string::npos) << jit.errors;
    EXPECT_GE(jit.stats.native_status_unwinds, 1u) << "the unwind left compiled code from inside the print";
    EXPECT_GE(jit.stats.output_calls, 1u);
  }
  // The same text with no scope limit: polls compared one by one, every one inside the print.
  {
    Scenario sc = scenario("use big; print(\"[\"); print(big()); print(\"]\");\n");
    sc.parts.push_back(Part{"big", std::string(20000, 'x') + "<% k = 0; %>tail", 100000, GLTANG_SCOPE_EMPTY, Mode::Template});
    std::vector<uint64_t> paced;
    sc.after = [&](Context & context) { paced.push_back(native_polls(context, "PRINT")); };
    Outcome plain, jit;
    expect_same(sc, &plain, &jit);
    ASSERT_FALSE(HasFailure());
    ASSERT_EQ(paced.size(), 2u);
    EXPECT_EQ(paced[0], paced[1]);
    EXPECT_GE(paced[1], 4u);
    EXPECT_TRUE(jit.raw == "[" + std::string(20000, 'x') + "tail]") << jit.raw.size() << " bytes";
  }
}

TEST(JitTemplates, ARefusedDeliveryIsRolledBackAndThePrintsValueIsHostFunctionFailed) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // The print is on a line of its own, so the origin of the error it makes names it.
  Scenario sc = scenario(
      "function f(n) {\n"
      "  r = null;\n"
      "  i = 0;\n"
      "  while (i < n) {\n"
      "    p = print(i);\n"
      "    if (i == 2) { r = p; }\n"
      "    i = i + 1;\n"
      "  }\n"
      "  return r;\n"
      "}\n"
      "x = f(5);\n"
      "print(x);\n"
      "x;\n");
  std::vector<Delivery> deliveries;
  deliveries.reserve(2);
  sc.before = [&](Context & context) {
    deliveries.emplace_back();
    deliveries.back().refuse_at = 2;
    ASSERT_EQ(gltang_execution_set_output_sink(context.execution, sink_fn, &deliveries.back()), GLTANG_OK);
  };
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  ASSERT_FALSE(HasFailure());
  ASSERT_EQ(deliveries.size(), 2u);
  EXPECT_EQ(jit.raw, "0134") << "the refused print was taken back and the run went on";
  EXPECT_EQ(deliveries[0].bytes, "0134");
  EXPECT_EQ(deliveries[1].bytes, deliveries[0].bytes);
  EXPECT_NE(jit.result.find("Host function failed"), std::string::npos) << jit.result;
  EXPECT_NE(jit.errors.find("page:5["), std::string::npos) << "the error's origin is the print's line: " << jit.errors;
  EXPECT_EQ(jit.stats.output_calls, 5u);
  EXPECT_EQ(jit.stats.deopts, kMain);
}

TEST(JitTemplates, ARawFloatPrintedIsTheDesignedExitAndPrintsTheInterpretersText) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // Compiled code never boxes (AD-27): the print of a raw float is an exit that boxes it
  // in the rebuild, and the interpreter prints it.
  Scenario sc = scenario("function f(n) { x = 1.5 * 2.0; i = 0; while (i < n) { i = i + 1; } print(x); print(\";\"); return i; }\nf(3);\nf(4);\n");
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(jit.raw, "3.;3.;");
  EXPECT_EQ(jit.stats.exits_expected, 2u) << "each call left at the print of the float, the designed exit";
  EXPECT_EQ(jit.stats.output_calls, 0u) << "the float was never given to the helper raw";
  EXPECT_GE(jit.stats.values_boxed, 2u);
  EXPECT_EQ(jit.stats.functions_discarded, 0u);
}

namespace {

/// A host-defined type with no operation, and the native that makes one.
struct Boxes {
  GLTANG_TypeRegistry * registry = nullptr;
  const GLTANG_Type * type = nullptr;
  Boxes() {
    EXPECT_EQ(gltang_type_registry_create(&registry), GLTANG_OK);
    GLTANG_TypeDescriptor d = GLTANG_TYPE_DESCRIPTOR_INIT;
    d.name = "box";
    d.slot_count = 1;
    d.thread_safe = true;
    EXPECT_EQ(gltang_type_registry_add(registry, &d, &type), GLTANG_OK);
  }
  ~Boxes() { gltang_type_registry_release(registry); }
  Boxes(const Boxes &) = delete;
  Boxes & operator=(const Boxes &) = delete;
  static bool make(GLTANG_NativeCall * call, void * user) {
    GLTANG_Handle h;
    if (gltang_handle_make_custom(call, static_cast<Boxes *>(user)->type, &h) != GLTANG_OK) {
      return false;
    }
    gltang_call_return_handle(call, h);
    return true;
  }
};

}  // namespace

TEST(JitTemplates, APrintOfAHostDefinedValueAppendsNothingAsInterpreted) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Boxes boxes;
  Scenario sc = scenario("function f(v) { r = print(v); print(\"|\"); return r; }\nuse t;\nx = f(t.box());\nx;\n");
  sc.before = [&](Context & context) {
    GLTANG_Library * t = nullptr;
    ASSERT_EQ(gltang_library_create("t", &t), GLTANG_OK);
    ASSERT_EQ(gltang_library_add_native(t, "box", &Boxes::make, &boxes), GLTANG_OK);
    ASSERT_EQ(gltang_library_add_library(context.library(), t), GLTANG_OK);
    gltang_library_release(t);
    ASSERT_EQ(gltang_execution_attach_types(context.execution, boxes.registry), GLTANG_OK);
  };
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(jit.raw, "|");
  EXPECT_EQ(jit.stats.output_calls, 2u);
  EXPECT_EQ(jit.stats.deopts, kMain);
}

TEST(JitTemplates, WhereNativesCannotBeCalledAPrintStaysAnExitAndTheOutputIsTheSame) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc = scenario("function f(n) { i = 0; while (i < n) { print(i); i = i + 1; } return i; }\nf(5);\nf(5);\n");
  sc.natives_off = true;
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(jit.raw, "0123401234");
  EXPECT_EQ(jit.stats.output_calls, 0u);
  EXPECT_EQ(jit.stats.deopts, kMain + 2u) << "each call left at its first print, as before the prints were compiled";
  EXPECT_EQ(jit.stats.exits_counted, kMain + 2u);
}

#else  // JIT=no

TEST(JitTemplates, WithoutTheJitPrintsRunInTheInterpreterAndNothingIsCompiled) {
  Compiled page("function f(n) { i = 0; while (i < n) { print(i); i = i + 1; } return i; }\nf(5);\n", Mode::Script, "nojit.tang");
  ASSERT_TRUE(page.ok());
  Context context(page.program);
  ASSERT_TRUE(context.ok());
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(context.raw(), "01234");
  GLTANG_JitStats st = context.jit_stats();
  EXPECT_EQ(st.output_calls + st.result_clears + st.functions_compiled, 0u);
}

#endif

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
