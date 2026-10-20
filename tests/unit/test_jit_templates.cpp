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

// The output operations in compiled code (spec-template-compiled story 1, CAP-1), and the
// end of a statement and of a template (story 2, CAP-2).
//
// `PRINT`, `PRINT_CONST` and `CLEAR_RESULT` run in compiled code as calls of the
// interpreter's own functions (`gltang_vm_op_print`, `gltang_vm_print_constant`,
// `gltang_vm_set_result`), so there is one implementation of printing. Each test runs
// one program interpreted and with every function compiled at its first poll, and
// requires the two runs to be the same run (output, result, error list, fuel, every
// poll and its identity; jit_harness.h), and then says in numbers that the prints ran
// compiled: the `output_calls` counter, and no exit.
//
// Story 2 compiles `SET_RESULT`, `DISCARD` and `STORE_GLOBAL`, and makes `HALT` a return
// (the interpreter then runs `HALT`'s body: the budget scope of a template call cannot
// close while compiled code is open). Before it, a script's top level left compiled code
// at its first function declaration (a global store), and a template's function 0 at its
// `HALT`, and the counts here allowed that one exit; now the top level and a template's
// function 0 run to their end compiled, and each count below is the exits a test makes
// beyond none.
//
// Story 3 compiles the value operations between the prints: a string constant, `+` past the
// small-integer path, `INDEX`, `SLICE` and the `ATTR` of a value that is not a library, each a
// call of the interpreter's own function; a host-defined operand of one is a real exit before it.
//
// Each can fail, and has been seen to: tests/planted/152 to 155, 157 and 158, 159 to 167 and 169
// to 173 here, 168 here under relocation torture, and 156 in testObserver (the frame
// differential over the output programs).

#include "exec_harness.h"
#include "jit_harness.h"
#include "test_helpers.h"

#include <ghoti.io/lang-tang/type.h>

#include <functional>
#include <set>
#include <string>
#include <vector>

using namespace jt;

#ifdef GLTANG_WITH_JIT

namespace {

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
  // left its operand in place would return 99. (A bare `print(i);`, which ends in
  // `DISCARD`, is story 2's test below.)
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
  EXPECT_EQ(jit.stats.deopts, 0u) << "no print left compiled code, and the top level ran to its end";
  EXPECT_EQ(jit.stats.functions_discarded, 0u);
  EXPECT_EQ(plain.stats.output_calls, 0u);
}

TEST(JitTemplates, ATemplateWhoseFirstOperationIsTextIsCompiledAndItsTextAndClearsRunCompiled) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // The function's first operation is `PRINT_CONST`, which the entry check used to
  // decline: the function was not compiled at all. Its `HALT` was an exit until story 2.
  Scenario sc = scenario("head<% if (1 < 2) { %>yes<% } else { %>no<% } %>tail", Mode::Template);
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(jit.raw, "headyestail");
  EXPECT_EQ(jit.stats.functions_compiled, 1u) << "the template's function 0 was compiled, not declined";
  EXPECT_EQ(jit.stats.entries, 1u);
  EXPECT_EQ(jit.stats.output_calls, 3u) << "the three texts were printed by compiled code";
  EXPECT_GE(jit.stats.result_clears, 1u) << "a text statement clears the template's result, in compiled code";
  EXPECT_EQ(jit.stats.deopts, 0u) << "the HALT is a return (story 2)";
  EXPECT_EQ(jit.stats.returns, 1u);
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
  EXPECT_EQ(jit.stats.deopts, 1u) << "the top level's one exit is its 1 / 0, where the interpreter makes the error";
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
    // Until story 3 the top level left at its first string concatenation (`s + s`); now the
    // concatenations are compiled calls too, so nothing leaves.
    EXPECT_EQ(jit.stats.deopts, 0u) << "the large print polled inside compiled code and did not leave it";
    EXPECT_EQ(jit.stats.value_calls, 12u) << "the string constant and the eleven `s + s` (`k += 1` is the small-integer path)";
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
    EXPECT_EQ(jit.raw, "[]") << "the scope ran out inside the text, which was rolled back";
    EXPECT_NE(jit.errors.find(std::to_string((int)GLTANG_ERROR_HOW_SCOPE_LIMIT) + ":"), std::string::npos) << jit.errors;
    // The template's one compiled operation that ran is its long text, and the unwind came out of it.
    EXPECT_EQ(jit.stats.output_calls, 1u);
    EXPECT_EQ(jit.stats.native_status_unwinds, 1u) << "the unwind left compiled code from inside the print";
    EXPECT_EQ(jit.stats.native_status_deopts, 0u);
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
    EXPECT_EQ(jit.raw, "[" + std::string(20000, 'x') + "tail]");
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
  EXPECT_EQ(jit.stats.output_calls, 6u) << "f's five, and the top level's print(x), compiled since story 2";
  EXPECT_EQ(jit.stats.deopts, 0u);
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
  EXPECT_EQ(jit.stats.deopts, 0u);
}

TEST(JitTemplates, WhereNativesCannotBeCalledAPrintStaysAnExitAndTheOutputIsTheSame) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // A print of a value and of a string literal (PRINT_CONST): both stay exits.
  {
    Scenario sc = scenario("function f(n) { i = 0; while (i < n) { r = print(i); print(\",\"); i = i + 1; } return i; }\nf(5);\nf(5);\n");
    sc.natives_off = true;
    Outcome plain, jit;
    expect_same(sc, &plain, &jit);
    ASSERT_FALSE(HasFailure());
    EXPECT_EQ(jit.raw, "0,1,2,3,4,0,1,2,3,4,");
    EXPECT_EQ(jit.raw, plain.raw);
    EXPECT_EQ(jit.stats.output_calls, 0u);
    EXPECT_EQ(jit.stats.deopts, 2u) << "each call left at its first print, as before the prints were compiled";
    EXPECT_EQ(jit.stats.exits_counted, 2u);
  }
  // A template whose code clears its result before its first text: CLEAR_RESULT is a plain
  // call and compiles even here, and the text is the exit.
  {
    Scenario sc = scenario("<% while (false) { } %>text<% while (false) { } %>more", Mode::Template);
    sc.natives_off = true;
    Outcome plain, jit;
    expect_same(sc, &plain, &jit);
    ASSERT_FALSE(HasFailure());
    EXPECT_EQ(jit.raw, "textmore");
    EXPECT_EQ(jit.raw, plain.raw);
    EXPECT_EQ(jit.stats.functions_compiled, 1u);
    EXPECT_EQ(jit.stats.output_calls, 0u);
    EXPECT_GE(jit.stats.result_clears, 1u) << "the clear before the text ran compiled";
    EXPECT_EQ(jit.stats.deopts, 1u) << "the one exit is the first text";
    EXPECT_EQ(jit.stats.exits_counted, 1u);
  }
}

// ---------------------------------------------------------------------------
// The end of a statement and of a template (story 2)
// ---------------------------------------------------------------------------

namespace {

/// A library `t` whose native `fail()` answers with an error value: the one way to make an
/// error inside compiled code with no exit (a guard exit, as `1 / 0` is, would leave the
/// rest of the function to the interpreter).
bool fail_native(GLTANG_NativeCall * call, void *) {
  gltang_call_return_error(call, GLTANG_ERROR_DIVIDE_BY_ZERO);
  return true;
}

/// `t.same(v)` answers with its argument, so an error made on one line can be the value of a
/// statement on another.
bool same_native(GLTANG_NativeCall * call, void *) {
  GLTANG_Handle h;
  if (gltang_call_arg(call, 0, &h) != GLTANG_OK) {
    return false;
  }
  gltang_call_return_handle(call, h);
  return true;
}

void add_failing_library(Context & context) {
  GLTANG_Library * t = nullptr;
  ASSERT_EQ(gltang_library_create("t", &t), GLTANG_OK);
  ASSERT_EQ(gltang_library_add_native(t, "fail", &fail_native, nullptr), GLTANG_OK);
  ASSERT_EQ(gltang_library_add_native(t, "same", &same_native, nullptr), GLTANG_OK);
  ASSERT_EQ(gltang_library_add_library(context.library(), t), GLTANG_OK);
  gltang_library_release(t);
}

/// A template part, `card`, called from the script (the execution's own name is `page`) with an ample scope.
Part card(const std::string & source) {
  return Part{"card", source, 100000, GLTANG_SCOPE_EMPTY, Mode::Template};
}

/// The template's function 0, as a program-qualified word (program 1 is the first part).
constexpr uint64_t kCard0 = uint64_t{1} << 32;

/// What `after` saw of the template's function 0: its counted exits, and whether it was still compiled.
struct CardState {
  int deopts = -2;
  int compiled = -1;
};

/// expect_same, and the position each run left recorded at its end (the interpreter's last SYNC,
/// compiled code's last stored site) is the same: an error made later with no SYNC of its own
/// would take it as its origin. The error list alone cannot see a wrong position stored by
/// DISCARD or HALT's body, since an entry carries the error's own origin.
void expect_same_position(Scenario sc, Outcome * plain, Outcome * jit) {
  std::vector<uint64_t> at;
  auto inner = sc.after;
  sc.after = [&at, inner](Context & context) {
    at.push_back(gltang_vm_test_position(context.execution));
    if (inner) {
      inner(context);
    }
  };
  expect_same(sc, plain, jit);
  ASSERT_EQ(at.size(), 2u);
  EXPECT_EQ(at[0], at[1]) << "the position recorded at the end of the run";
}

std::function<void(Context &)> watch_card(std::vector<CardState> * seen) {
  return [seen](Context & context) {
    CardState st;
    st.deopts = gltang_vm_jit_test_function_deopts(context.execution, kCard0);
    st.compiled = gltang_vm_jit_test_discard(context.execution, kCard0);  // last: it lets the code go
    seen->push_back(st);
  };
}

}  // namespace

TEST(JitTemplates, AWarmTemplateOfTextAndAnExpressionIsCalledAHundredTimesWithNoExitAndIsNeverDiscarded) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // The template's function 0 is text, `<%= %>` (PRINT, then SET_RESULT 1) and an assignment
  // to a top-level variable (STORE_GLOBAL, SET_RESULT 0), and ends at its HALT. Before story 2
  // each call left compiled code at a SET_RESULT, the store or the HALT, every one a counted
  // exit, and at the eighth the function was discarded and the calls ran interpreted.
  Scenario sc = scenario("use card; i = 0; while (i < 100) { print(card()); i = i + 1; }\n");
  sc.parts.push_back(card("<% x = 40 + 2; %><p><%= x %></p>\n"));
  std::vector<CardState> seen;
  sc.after = watch_card(&seen);
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  ASSERT_FALSE(HasFailure());
  std::string expected;
  for (int i = 0; i < 100; ++i) {
    expected += "<p>42</p>\n";
  }
  EXPECT_EQ(jit.raw, expected);
  ASSERT_EQ(seen.size(), 2u);
  EXPECT_EQ(seen[1].deopts, 0) << "no exit was charged to the template's function 0";
  EXPECT_EQ(seen[1].compiled, 1) << "it was still compiled after a hundred calls";
  EXPECT_EQ(jit.stats.exits_expected, 0u);
  // Template calls from compiled code stay exits (the calls spec's decision): the script's
  // top level, entered once, leaves at its first template call and the interpreter runs the
  // rest of it. That is the run's one exit, and it is the script's.
  EXPECT_EQ(jit.stats.deopts, 1u) << "the script's first template call";
  EXPECT_EQ(jit.stats.exits_counted, 1u);
  EXPECT_EQ(jit.stats.functions_discarded, 0u);
  EXPECT_GE(jit.stats.entries, 100u) << "the template's function 0 was entered at every call";
  EXPECT_GE(jit.stats.returns, 100u) << "and every call returned at its HALT, counted as a return";
  EXPECT_GE(jit.stats.output_calls, 300u) << "its two texts and its print, each call";
}

TEST(JitTemplates, TopLevelVariablesAreStoredAndTheResultSetInCompiledCode) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // A script's top level: declarations and assignments are STORE_GLOBAL, expression
  // statements SET_RESULT, and the program's result is the last one. It runs to its HALT
  // compiled.
  {
    Scenario sc = scenario("function f(a) { return a * 3; }\nn = 4; m = f(n); n + m; m;\n");
    Outcome plain, jit;
    expect_same(sc, &plain, &jit);
    ASSERT_FALSE(HasFailure());
    EXPECT_EQ(jit.result, plain.result);
    EXPECT_NE(jit.result.find("12"), std::string::npos) << jit.result;
    EXPECT_EQ(jit.stats.deopts, 0u) << "the stores, the results and the HALT made no exit";
    EXPECT_EQ(jit.stats.returns, 1u) << "the top level's HALT, a return to the interpreter (f returned to compiled code)";
    EXPECT_EQ(jit.stats.calls, 1u);
  }
  // A template's own top-level variables, read back by `<%= %>`.
  {
    Scenario sc = scenario("use card; print(card()); print(card());\n");
    sc.parts.push_back(card("<% n = 3; m = n * 2; %>[<%= n %>,<%= m %>]"));
    std::vector<CardState> seen;
    sc.after = watch_card(&seen);
    Outcome plain, jit;
    expect_same(sc, &plain, &jit);
    ASSERT_FALSE(HasFailure());
    EXPECT_EQ(jit.raw, "[3,6][3,6]");
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[1].deopts, 0);
    EXPECT_EQ(seen[1].compiled, 1);
  }
  // `n = items.length`: the string constant and the attribute read were exits until story 3,
  // and what followed ran in the interpreter; now both are compiled calls and the template's
  // function 0 runs to its end.
  {
    Scenario sc = scenario("use card; print(card()); print(card());\n");
    sc.parts.push_back(card("<% items = \"abc\"; n = items.length; %><%= n %>"));
    std::vector<CardState> seen;
    sc.after = watch_card(&seen);
    Outcome plain, jit;
    expect_same(sc, &plain, &jit);
    ASSERT_FALSE(HasFailure());
    EXPECT_EQ(jit.raw, "33");
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[1].deopts, 0) << "the constant and the attribute ran compiled";
    EXPECT_EQ(seen[1].compiled, 1);
    EXPECT_EQ(jit.stats.value_calls, 4u) << "the constant and `.length`, at each of the two calls";
  }
}

TEST(JitTemplates, AnErrorResultThatALaterStatementReplacesIsRecordedAsDiscardedFromCompiledCode) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // `t.fail();` is a listed expression statement: its error is the template's result and
  // is marked lost (the compiled SET_RESULT 1). The text after it clears the result (the
  // compiled CLEAR_RESULT, story 1's carried test), which records the lost error as
  // discarded, with the template's name and the chain of calls above it.
  Scenario sc = scenario("use card; print(card());\nprint(card());\n");
  sc.parts.push_back(card("<% use t; t.fail(); %>text"));
  sc.before = add_failing_library;
  std::vector<CardState> seen;
  sc.after = watch_card(&seen);
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(jit.raw, "texttext");
  const std::string discarded = "card:1[page:1]" + std::to_string((int)GLTANG_ERROR_HOW_DISCARDED) + ":";
  EXPECT_NE(jit.errors.find(discarded), std::string::npos) << jit.errors;
  EXPECT_EQ(jit.errors.find(std::to_string((int)GLTANG_ERROR_HOW_TEMPLATE_RESULT) + ":"), std::string::npos)
      << "the result at the end was the text's null, not the error: " << jit.errors;
  ASSERT_EQ(seen.size(), 2u);
  EXPECT_EQ(seen[1].deopts, 0) << "the error was made, set, replaced and recorded in compiled code";
  EXPECT_GE(jit.stats.native_calls, 2u) << "fail() was called from compiled code";
}

TEST(JitTemplates, AnErrorResultThatAVariableHoldsIsNotLostAndNotRecordedAsDiscarded) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // `x = t.fail();` and the bare `x;` are unlisted statements (SET_RESULT 0): the error is
  // held by a variable, so it is not marked lost, and the text's clear records nothing. A
  // compiled SET_RESULT that took every result as listed would record it (planted 166).
  for (const char * body : {"<% use t; x = t.fail(); %>text", "<% use t; x = t.fail(); x; %>text"}) {
    Scenario sc = scenario("use card; print(card());\nprint(card());\n");
    sc.parts.push_back(card(body));
    sc.before = add_failing_library;
    std::vector<CardState> seen;
    sc.after = watch_card(&seen);
    Outcome plain, jit;
    expect_same(sc, &plain, &jit);
    ASSERT_FALSE(HasFailure()) << body;
    EXPECT_EQ(jit.raw, "texttext") << body;
    EXPECT_EQ(jit.errors, plain.errors) << body;
    EXPECT_EQ(jit.errors.find(std::to_string((int)GLTANG_ERROR_HOW_DISCARDED) + ":"), std::string::npos) << body << ": " << jit.errors;
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[1].deopts, 0) << body << ": the statements ran compiled";
    EXPECT_EQ(seen[1].compiled, 1) << body;
  }
}

TEST(JitTemplates, AnErrorThatIsATemplatesFinalResultIsRecordedAsLostAcrossTheTemplateBoundary) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // The template's last statement leaves an error as its result; the HALT returns from
  // compiled code, and `HALT`'s body, run by the interpreter after the return, records it
  // as lost across the template boundary (HOW_TEMPLATE_RESULT) with its chain.
  Scenario sc = scenario("use card;\nprint(\"[\" + card() + \"]\");\n");
  sc.parts.push_back(card("before<% use t; t.fail(); %>"));
  sc.before = add_failing_library;
  std::vector<CardState> seen;
  sc.after = watch_card(&seen);
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(jit.raw, "[before]");
  const std::string lost = "card:1[page:2]" + std::to_string((int)GLTANG_ERROR_HOW_TEMPLATE_RESULT) + ":";
  EXPECT_NE(jit.errors.find(lost), std::string::npos) << jit.errors;
  ASSERT_EQ(seen.size(), 2u);
  EXPECT_EQ(seen[1].deopts, 0);
  EXPECT_EQ(seen[1].compiled, 1);
}

TEST(JitTemplates, ABareStatementInAFunctionIsDiscardedInCompiledCodeAndTheFunctionIsNotDiscarded) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // `print(i);` in a function ends in DISCARD; called twenty times, its loop of bare prints
  // makes no exit, so the function is not discarded at its eighth call.
  {
    Scenario sc = scenario("function f(n) { i = 0; while (i < n) { print(i); i = i + 1; } return i; }\n"
                           "k = 0; while (k < 20) { f(3); k = k + 1; }\n");
    Outcome plain, jit;
    expect_same(sc, &plain, &jit);
    ASSERT_FALSE(HasFailure());
    std::string expected;
    for (int k = 0; k < 20; ++k) {
      expected += "012";
    }
    EXPECT_EQ(jit.raw, expected);
    EXPECT_EQ(jit.stats.deopts, 0u) << "no exit at a DISCARD";
    EXPECT_EQ(jit.stats.functions_discarded, 0u);
    EXPECT_EQ(jit.stats.output_calls, 60u);
  }
  // A discarded error, made on line 3 and discarded by the bare statement on line 4: entered
  // in the error list from compiled code, with the error's own origin (line 3), and the
  // statement's position recorded as the interpreter's DISCARD records it (planted 164).
  {
    // Three calls and no loop: no poll after the last DISCARD records another position.
    Scenario sc = scenario("function g() {\n  use t;\n  e = t.fail();\n  t.same(e);\n  return 1;\n}\ng(); g(); g();\n");
    sc.before = add_failing_library;
    Outcome plain, jit;
    expect_same_position(sc, &plain, &jit);
    ASSERT_FALSE(HasFailure());
    const std::string discarded = std::to_string((int)GLTANG_ERROR_HOW_DISCARDED) + ":";
    size_t count = 0;
    for (size_t at = jit.errors.find(discarded); at != std::string::npos; at = jit.errors.find(discarded, at + 1)) {
      ++count;
    }
    EXPECT_EQ(count, 3u) << jit.errors;
    EXPECT_NE(jit.errors.find("page:3["), std::string::npos) << "each at the error's origin, the line that made it: " << jit.errors;
    EXPECT_EQ(jit.errors.find("page:4["), std::string::npos) << "not the line that discarded it: " << jit.errors;
    EXPECT_EQ(jit.stats.native_calls, 6u) << "fail() and same() were called from compiled code";
    EXPECT_EQ(jit.stats.deopts, 0u);
  }
}

TEST(JitTemplates, AScriptsTopLevelAndATemplateModeRunEndAtTheirHaltAsInterpreted) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // The main activation's HALT: the run finishes, with its result, output and fuel.
  for (const char * source : {"x = 5; x + 1;\n", "print(1); return 7;\n", "while (true) { break; }\n", "print(2); x = 3;\n"}) {
    Scenario sc = scenario(source);
    Outcome plain, jit;
    expect_same(sc, &plain, &jit);
    ASSERT_FALSE(HasFailure()) << source;
    EXPECT_TRUE(jit.finished) << source;
    EXPECT_EQ(jit.fuel, plain.fuel) << source;
    EXPECT_EQ(jit.stats.deopts, 0u) << source;
    EXPECT_EQ(jit.stats.returns, 1u) << source << ": the HALT is a return";
  }
  // A Template mode run is the main activation too.
  {
    Scenario sc = scenario("head<% x = 2; %><%= x * 2 %>tail", Mode::Template);
    Outcome plain, jit;
    expect_same(sc, &plain, &jit);
    ASSERT_FALSE(HasFailure());
    EXPECT_EQ(jit.raw, "head4tail");
    EXPECT_EQ(jit.stats.deopts, 0u);
    EXPECT_EQ(jit.stats.returns, 1u);
  }
}

TEST(JitTemplates, ARawFloatStoredMadeTheResultOrDiscardedIsTheDesignedBoxExitOfThatOperation) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // `1.5 * 2.0` is a raw double in compiled code. Stored in a top-level variable
  // (STORE_GLOBAL), made the statement result (SET_RESULT) or discarded (DISCARD), it is the
  // box exit at that operation, as story 1's print of one is, and the interpreter runs the
  // operation with the box. Each operation in a scenario of its own, with exact counts; the
  // loop before it keeps the function from the static decline (a function whose every path
  // ends at a box exit is not compiled at all). Planted 165 takes STORE_GLOBAL off the list.
  struct Case {
    const char * name;
    const char * source;
    const char * template_source;
    uint64_t expected, counted;
  };
  const Case cases[] = {
      // The template is entered by the interpreter at each of its two calls (the script's
      // first template call is its one exit, counted) and leaves at its store each time.
      {"STORE_GLOBAL", "use card; print(card()); print(card());\n",
       "<% i = 0; while (i < 2) { i = i + 1; } y = 1.5 * 2.0; %><%= y %>", 2, 1},
      // The top level leaves at its last statement's result, once.
      {"SET_RESULT", "i = 0; while (i < 2) { i = i + 1; } 2.5 * 2.0;\n", nullptr, 1, 0},
      // f leaves at its discarded float at each call: the first from the compiled top level
      // (the chain is rebuilt), the second from the interpreter.
      {"DISCARD", "function f() { i = 0; while (i < 2) { i = i + 1; } 1.5 * 2.0; return 1; }\nf(); f();\n", nullptr, 2, 0},
  };
  for (const Case & c : cases) {
    Scenario sc = scenario(c.source);
    if (c.template_source) {
      sc.parts.push_back(card(c.template_source));
    }
    Outcome plain, jit;
    expect_same(sc, &plain, &jit);
    ASSERT_FALSE(HasFailure()) << c.name;
    EXPECT_EQ(jit.result, plain.result) << c.name;
    EXPECT_EQ(jit.stats.exits_expected, c.expected) << c.name << ": one box exit per run of the operation";
    EXPECT_EQ(jit.stats.values_boxed, c.expected) << c.name << ": the one raw value, boxed at each";
    EXPECT_EQ(jit.stats.exits_counted, c.counted) << c.name;
    EXPECT_EQ(jit.stats.deopts, c.expected + c.counted) << c.name;
    EXPECT_EQ(jit.stats.functions_declined_expected, 0u) << c.name << ": the operation was compiled, not declined";
  }
}

TEST(JitTemplates, AFailureToBuildATemplatesOutputIsTheInterpretersAtEveryMemoryBudget) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // The output string is built by HALT's body after compiled code returned: it allocates
  // (the string, its grapheme offsets and the sink's buffer), and a memory budget that runs
  // out there makes the call's value the interpreter's `Out of memory`. The template's text
  // is 100000 bytes, so printing it charges half the run's fuel and building the result the
  // other half (both are paced by bytes): an `Out of memory` after more than half the fuel
  // is a failure while the result was built. Swept from far below the run's peak to above
  // it, the tiers agree at every budget, and the sweep sees that case.
  Scenario sc = scenario("use card; a = card(); print(\"|\"); a;\n");
  sc.parts.push_back(card(std::string(100000, 'y') + "<% k = 1; %>"));
  sc.memory_reserve = 0;
  Outcome measured = run(sc, 0);
  Outcome compiled_measured = run(sc, 1);
  ASSERT_TRUE(measured.finished);
  ASSERT_TRUE(compiled_measured.finished);
  // The compiled run is one: the script's top level is entered and leaves at its template
  // call (its one counted exit), and the template's function 0 is entered and returns at its
  // HALT, so the sweep does not compare the interpreter with itself.
  EXPECT_EQ(compiled_measured.stats.entries, 2u) << "the top level and the template's function 0";
  EXPECT_EQ(compiled_measured.stats.returns, 1u) << "the template returned at its HALT";
  EXPECT_EQ(compiled_measured.stats.deopts, 1u);
  EXPECT_EQ(compiled_measured.stats.exits_counted, 1u);
  const uint64_t low = std::min(measured.memory_peak, compiled_measured.memory_peak) / 8;
  const uint64_t high = std::max(measured.memory_peak, compiled_measured.memory_peak) + 2000;
  std::set<std::string> kinds;
  bool failed_building = false;
  for (uint64_t i = 0; i < 64; ++i) {
    Scenario tight = sc;
    tight.memory_bytes = low + (high - low) * i / 63;
    Outcome plain = run(tight, 0);
    Outcome jit = run(tight, 1);
    kinds.insert(plain.key());
    EXPECT_EQ(plain.key(), jit.key()) << "memory " << tight.memory_bytes;
    EXPECT_EQ(plain.polls, jit.polls) << "memory " << tight.memory_bytes;
    failed_building = failed_building || (plain.result.find("Out of memory") != std::string::npos && plain.fuel > measured.fuel / 2 && plain.fuel < measured.fuel);
  }
  EXPECT_GE(kinds.size(), 3u) << "the sweep did not cross both of the budgets it is meant to";
  EXPECT_TRUE(failed_building) << "no budget ran out while the result was built";
}

TEST(JitTemplates, EveryFuelBudgetAcrossATemplateCallStopsAtTheSamePollOnBothTiers) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // A run that stops at each budget in turn, from too little to enough: the pause (its
  // location, identity and fuel) is the same on both tiers, before, inside and after the
  // template's compiled function 0 and its HALT. The template spans four lines, and its
  // HALT is on the last, apart from its first instruction's.
  Scenario sc = scenario("use card; i = 0; while (i < 3) { print(card()); i = i + 1; }\n");
  sc.parts.push_back(card("<% x = 1;\nwhile (x < 4) {\n  x = x + 1;\n} %>[<%= x %>]\n"));
  Outcome whole = run(sc, 0);
  ASSERT_TRUE(whole.finished);
  ASSERT_GT(whole.fuel, 20u);
  {
    std::vector<CardState> seen;
    Scenario watched = sc;
    watched.after = watch_card(&seen);
    Outcome compiled = run(watched, 1);
    ASSERT_TRUE(compiled.finished);
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].deopts, 0) << "the template ran compiled to its HALT";
    EXPECT_EQ(seen[0].compiled, 1);
    EXPECT_EQ(compiled.stats.entries, 4u) << "the top level, and the template's function 0 at each of its three calls";
    EXPECT_EQ(compiled.stats.returns, 3u) << "each call returned at its HALT";
  }
  for (uint64_t fuel = 1; fuel <= whole.fuel + 1; ++fuel) {
    Scenario tight = sc;
    tight.fuel = fuel;
    Outcome plain = run(tight, 0);
    Outcome jit = run(tight, 1);
    ASSERT_EQ(plain.key(), jit.key()) << "fuel " << fuel;
    ASSERT_EQ(plain.polls, jit.polls) << "fuel " << fuel;
  }
}

TEST(JitTemplates, HaltsBodyRecordsTheHaltsPositionAfterACompiledReturn) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // A template call's HALT records where it is before its body runs (the interpreter's
  // SYNC), and a compiled HALT passes that index in its halted word. Nothing after the
  // call records another position here (the script's top level stores a result and halts),
  // so the position the run ends with is the template HALT's, on both tiers (planted 167).
  Scenario sc = scenario("use card;\nx = card();\n");
  sc.parts.push_back(card("<% a = 1;\nb = a + 1;\n%>[<%= b %>]\n"));
  std::vector<CardState> seen;
  sc.after = watch_card(&seen);
  Outcome plain, jit;
  expect_same_position(sc, &plain, &jit);
  ASSERT_FALSE(HasFailure());
  ASSERT_EQ(seen.size(), 2u);
  EXPECT_EQ(seen[1].deopts, 0);
  EXPECT_EQ(jit.stats.returns, 1u) << "the template returned at its HALT";
}

TEST(JitTemplates, TheTopLevelExitTheOtherSuitesPutFirstStillLeavesOnceAndCounts) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // kTopLevelLeaves (jit_harness.h) alone: the top level leaves compiled code once, a counted
  // exit charged to function 0, and then ends in the interpreter. It is not free: it declares a
  // global (`leave`, slot 0 of a program that begins with it) and adds its own fuel and polls
  // (the loop's), on both tiers alike, which the suites that use it count as before. Planted
  // 162's test (the top-level variables) does not use it, so 162 does not depend on what slot 0
  // holds.
  Scenario sc = scenario(kTopLevelLeaves);
  int main_deopts = -2;
  sc.after = [&main_deopts](Context & context) { main_deopts = gltang_vm_jit_test_function_deopts(context.execution, 0); };
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(jit.stats.entries, 1u);
  EXPECT_EQ(jit.stats.deopts, 1u);
  EXPECT_EQ(jit.stats.exits_counted, 1u);
  EXPECT_EQ(jit.stats.exits_expected, 0u);
  EXPECT_EQ(jit.stats.returns, 0u) << "the HALT ran in the interpreter";
  EXPECT_EQ(main_deopts, 1) << "the exit is charged to function 0";
}

// ---------------------------------------------------------------------------
// The value operations (story 3)
// ---------------------------------------------------------------------------

namespace {

/// `t.items(n)`: an array of n maps, each `{name: "<item k>" (tagged HTML), row: [k, "r<k>"]}`,
/// made by the host, so that a template reads maps and arrays with no `MAP` or `ARRAY` of its own
/// (those are story 4's, and an exit until then).
bool items_native(GLTANG_NativeCall * call, void *) {
  GLTANG_Handle count;
  if (gltang_call_arg(call, 0, &count) != GLTANG_OK) {
    return false;
  }
  const int64_t n = gltang_handle_integer(call, count);
  GLTANG_Handle array;
  if (gltang_handle_make_array(call, static_cast<size_t>(n), &array) != GLTANG_OK) {
    return false;
  }
  for (int64_t k = 0; k < n; ++k) {
    if (gltang_handle_push_frame(call) != GLTANG_OK) {
      return false;
    }
    const std::string name = "<item " + std::to_string(k) + ">";
    const std::string cell = "r" + std::to_string(k);
    GLTANG_Handle map, text, row, number, second;
    bool ok = gltang_handle_make_map(call, 2, &map) == GLTANG_OK
        && gltang_handle_make_string(call, name.data(), name.size(), GLTANG_UNICODE_STRING_TYPE_HTML, &text) == GLTANG_OK
        && gltang_handle_map_set(call, map, "name", 4, GLTANG_UNICODE_STRING_TYPE_TRUSTED, text) == GLTANG_OK
        && gltang_handle_make_array(call, 2, &row) == GLTANG_OK
        && gltang_handle_make_integer(call, k, &number) == GLTANG_OK
        && gltang_handle_array_push(call, row, number) == GLTANG_OK
        && gltang_handle_make_string(call, cell.data(), cell.size(), GLTANG_UNICODE_STRING_TYPE_TRUSTED, &second) == GLTANG_OK
        && gltang_handle_array_push(call, row, second) == GLTANG_OK
        && gltang_handle_map_set(call, map, "row", 3, GLTANG_UNICODE_STRING_TYPE_TRUSTED, row) == GLTANG_OK
        && gltang_handle_array_push(call, array, map) == GLTANG_OK;
    if (gltang_handle_pop_frame(call) != GLTANG_OK || !ok) {
      return false;
    }
  }
  gltang_call_return_handle(call, array);
  return true;
}

void add_items_library(Context & context) {
  GLTANG_Library * t = nullptr;
  ASSERT_EQ(gltang_library_create("t", &t), GLTANG_OK);
  ASSERT_EQ(gltang_library_add_native(t, "items", &items_native, nullptr), GLTANG_OK);
  ASSERT_EQ(gltang_library_add_library(context.library(), t), GLTANG_OK);
  gltang_library_release(t);
}

/// The template the spec is about: text, `<%= item.name %>` (a map's attribute), `<%= row[0] %>`
/// (an array's element) and a concatenation, in a loop over the host's items.
const char * const kItemsTemplate =
    "<% use t; items = t.items(5); n = items.size; i = 0; while (i < n) { item = items[i]; row = item.row; %>"
    "<li><%= item.name %>|<%= row[0] %>|<%= \"<b>\" + row[1] + \"</b>\" %></li>\n<% i += 1; } %>";

}  // namespace

TEST(JitTemplates, ATemplateOfAttributesElementsAndConcatenationsIsCalledManyTimesWithNoExitAndIsNeverDiscarded) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // Before story 3 each call left compiled code at its first string constant, attribute,
  // index or concatenation, and at the eighth call the function was discarded.
  Scenario sc = scenario("use card; i = 0; while (i < 40) { print(card()); i = i + 1; }\n");
  sc.parts.push_back(card(kItemsTemplate));
  sc.before = add_items_library;
  std::vector<CardState> seen;
  sc.after = watch_card(&seen);
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  ASSERT_FALSE(HasFailure());
  EXPECT_NE(jit.raw.find("<li><item 3>|3|<b>r3</b></li>"), std::string::npos) << jit.raw.substr(0, 200);
  EXPECT_NE(jit.rendered.find("&lt;item 3&gt;"), std::string::npos) << "the name keeps its tag: " << jit.rendered.substr(0, 200);
  ASSERT_EQ(seen.size(), 2u);
  EXPECT_EQ(seen[1].deopts, 0) << "no exit was charged to the template's function 0";
  EXPECT_EQ(seen[1].compiled, 1) << "it was still compiled after forty calls";
  EXPECT_EQ(jit.stats.functions_discarded, 0u);
  EXPECT_EQ(jit.stats.deopts, 1u) << "the one exit is the script's first template call";
  EXPECT_EQ(jit.stats.exits_counted, 1u);
  // Each call: `.size`, and five times `items[i]`, `.row`, `.name`, `row[0]`, `row[1]`, and the
  // two string constants and two `+` of the concatenation: 1 + 5 * 9.
  EXPECT_EQ(jit.stats.value_calls, 40u * (1u + 5u * 9u));
  EXPECT_EQ(plain.stats.value_calls, 0u);
}

TEST(JitTemplates, AConcatenationInALoopRunsCompiledWithTheInterpretersOutputAndFuel) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // Strings, a string and an integer (converted as printing it would), and arrays.
  Scenario sc = scenario(
      "function f(n) { s = \"\"; a = null; i = 0; while (i < n) { s = s + \"<li>\" + i + \"</li>\"; i = i + 1; } return s; }\n"
      "function g(x, y) { return x + y; }\n"
      "use t; a = t.items(2); b = g(a, a); x = f(30); print(x); print(f(3)); print(b.size); print(g(\"n=\", 3.5));\n");
  sc.before = add_items_library;
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  ASSERT_FALSE(HasFailure());
  EXPECT_NE(jit.raw.find("<li>29</li><li>0</li><li>1</li><li>2</li>4n=3.5"), std::string::npos) << jit.raw;
  EXPECT_EQ(jit.stats.deopts, 0u) << "no concatenation left compiled code";
  EXPECT_GE(jit.stats.value_calls, 33u * 4u) << "each iteration's two constants and three `+`, and g's";
  EXPECT_EQ(jit.fuel, plain.fuel);
}

TEST(JitTemplates, ALongConcatenationAndALongSlicePollAsOftenAndAScopeThatRunsOutInsideOneStopsAtTheSamePoll) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // 20480 bytes, then a concatenation of 40961 and slices of a string (by a step, reversed) and of
  // an array, each past the pacing interval, inside compiled functions: every poll inside them
  // is compared with its identity, and the pacing polls counted by native.
  {
    Scenario sc = scenario(
        "function cat(s) { r = s + \"|\" + s; return r; }\n"
        "function sl(s, a) { b = s[::-1]; c = s[1:20000:3]; d = a[::2]; return b.length + c.length + d.size; }\n"
        "s = \"0123456789\"; for (k = 0; k < 11; k += 1) { s = s + s; }\n"
        "use t; a = t.items(1)[0].row * 3000;\n"
        "x = cat(s); print(x.length); y = sl(s, a); print(y);\n");
    sc.before = add_items_library;
    std::vector<uint64_t> concat, string_slice, array_slice;
    sc.after = [&](Context & context) {
      concat.push_back(native_polls(context, "STRING_CONCAT"));
      string_slice.push_back(native_polls(context, "STRING_SLICE"));
      array_slice.push_back(native_polls(context, "ARRAY_SLICE"));
    };
    Outcome plain, jit;
    expect_same(sc, &plain, &jit);
    ASSERT_FALSE(HasFailure());
    EXPECT_EQ(jit.raw, "40961" + std::to_string(20480 + 6667 + 3000));
    ASSERT_EQ(concat.size(), 2u);
    EXPECT_EQ(concat[0], concat[1]);
    EXPECT_EQ(string_slice[0], string_slice[1]);
    EXPECT_EQ(array_slice[0], array_slice[1]);
    EXPECT_GE(concat[1], 10u);
    EXPECT_GE(string_slice[1], 10u);
    EXPECT_GE(array_slice[1], 1u);
    EXPECT_EQ(jit.stats.functions_discarded, 0u);
    // The top level's one exit is its array repeat (`* 3000`, an array operand of `*`).
    EXPECT_EQ(jit.stats.deopts, 1u);
  }
  // A template whose scope runs out inside one of its long concatenations: the helper reports
  // the unwind, compiled code leaves through the chain deopt, and the run goes on as interpreted.
  {
    Scenario sc = scenario("use big; print(\"[\"); r = big(); print(\"]\"); print(r);\n");
    sc.parts.push_back(Part{"big", "<% s = \"0123456789\"; k = 0; while (k < 13) { s = s + s; k += 1; } %><%= s.length %>", 400,
        GLTANG_SCOPE_EMPTY, Mode::Template});
    Outcome plain, jit;
    expect_same(sc, &plain, &jit);
    ASSERT_FALSE(HasFailure());
    EXPECT_EQ(jit.raw.substr(0, 2), "[]") << jit.raw;
    EXPECT_NE(jit.errors.find(std::to_string((int)GLTANG_ERROR_HOW_SCOPE_LIMIT) + ":"), std::string::npos) << jit.errors;
    EXPECT_EQ(jit.stats.native_status_unwinds, 1u) << "the unwind left compiled code from inside the concatenation";
  }
}

TEST(JitTemplates, AStringsIndexAndSliceAreTheInterpretersGraphemesInCompiledCode) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc = scenario(
      "function g(s) { a = s[1]; b = s[1:7]; c = s[::-1]; d = s[-1]; e = s[2:]; return a + \"|\" + b + \"|\" + c + \"|\" + d + \"|\" + e; }\n"
      "s = \"h\xc3\xa9llo\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbdw\xc3\xb6rld\"; print(g(s)); print(\";\"); print(g(s));\n");
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(jit.raw, plain.raw);
  EXPECT_NE(jit.raw.find("\xc3\xa9|\xc3\xa9llo\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbdw|"), std::string::npos) << "the emoji and its modifier are one grapheme: " << jit.raw;
  EXPECT_EQ(jit.stats.deopts, 0u);
}

TEST(JitTemplates, ARetagAndARenderInCompiledCodeKeepTheInterpretersSegmentsAndTags) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc = scenario(
      "function h(s) { x = s.html; y = (x + \"<i>\").render; z = s.html_attribute + s.percent + s.javascript + s.raw; return x + \"/\" + y + \"/\" + z + \"/\" + x.byte_length; }\n"
      "print(h(\"<b>&\")); print(h(\"q\\\"r\"));\n");
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(jit.rendered, plain.rendered);
  EXPECT_NE(jit.rendered.find("&lt;b&gt;&amp;"), std::string::npos) << jit.rendered;
  EXPECT_EQ(jit.stats.deopts, 0u);
  EXPECT_GE(jit.stats.value_calls, 2u * 9u);
}

TEST(JitTemplates, ValueOperationErrorsAreTheInterpretersWithTheirOrigins) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // Each on a line of its own, so each error's origin names it: `null + "x"`, an array indexed
  // by a string, an integer indexed, an index past the end (null, no error), a slice of an
  // integer and a slice by a string, an attribute a string does not have.
  Scenario sc = scenario(
      "function f(a, s) {\n"
      "  e1 = null + s;\n"
      "  e2 = a[s];\n"
      "  e3 = 5[0];\n"
      "  e4 = a[5];\n"
      "  e5 = 5[1:2];\n"
      "  e6 = s[s:];\n"
      "  e7 = s.nothing;\n"
      "  print(e1); print(e2); print(e3); print(e4); print(e5); print(e6); print(e7);\n"
      "  return 1;\n"
      "}\n"
      "use t; a = t.items(2); f(a, \"x\"); f(a, \"y\");\n");
  sc.before = add_items_library;
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(jit.errors, plain.errors);
  for (const char * text : {"Not supported", "Invalid index", "Not implemented"}) {
    EXPECT_NE(jit.errors.find(text), std::string::npos) << text << ": " << jit.errors;
  }
  for (int line = 2; line <= 8; ++line) {
    if (line == 5) {
      continue;  // a[5] is null
    }
    EXPECT_NE(jit.errors.find("page:" + std::to_string(line) + "["), std::string::npos) << "line " << line << ": " << jit.errors;
  }
  EXPECT_EQ(jit.stats.deopts, 0u) << "every error was made by the interpreter's own operation, called from compiled code";
}

TEST(JitTemplates, AHostDefinedOperandOfAValueOperationIsARealExitBeforeTheOperation) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // The custom dispatch is host-extensibility story 10's: until then each of these leaves
  // compiled code before the operation, with nothing of it done, and the interpreter asks the
  // type (a type with no operations: the interpreter's answer). Each exit counts toward the
  // discard limit, as a guard's does, and is not taken for a native-stack exit.
  Boxes boxes;
  struct Case {
    const char * name;
    const char * body;
    uint64_t constants;  // the calls of the string constant helper before the operation
  };
  const Case cases[] = {
      {"attribute", "return m.amount;", 0},
      {"index", "return m[0];", 0},
      {"slice", "return m[1:2];", 0},
      {"left of +", "return m + 1;", 0},
      {"right of +", "return \"x\" + m;", 3},
  };
  for (const Case & c : cases) {
    Scenario sc = scenario(std::string(kTopLevelLeaves) + "function f(m) { " + c.body + " }\nuse t; b = t.box(); f(b); f(b); f(b);\n");
    sc.before = [&](Context & context) {
      GLTANG_Library * t = nullptr;
      ASSERT_EQ(gltang_library_create("t", &t), GLTANG_OK);
      ASSERT_EQ(gltang_library_add_native(t, "box", &Boxes::make, &boxes), GLTANG_OK);
      ASSERT_EQ(gltang_library_add_library(context.library(), t), GLTANG_OK);
      gltang_library_release(t);
      ASSERT_EQ(gltang_execution_attach_types(context.execution, boxes.registry), GLTANG_OK);
    };
    int f_deopts = -2;
    sc.after = [&f_deopts](Context & context) { f_deopts = gltang_vm_jit_test_function_deopts(context.execution, 1); };
    Outcome plain, jit;
    expect_same(sc, &plain, &jit);
    ASSERT_FALSE(HasFailure()) << c.name;
    EXPECT_EQ(jit.result, plain.result) << c.name;
    EXPECT_EQ(f_deopts, 3) << c.name << ": one counted exit at each call, charged to f";
    EXPECT_EQ(jit.stats.exits_counted, 1u + 3u) << c.name << ": the top level's, and f's three";
    EXPECT_EQ(jit.stats.native_exits_stack, 0u) << c.name << ": not taken for a refused native call";
    EXPECT_EQ(jit.stats.value_calls, c.constants) << c.name << ": the operation's helper was never given the host-defined value";
  }
}

TEST(JitTemplates, ARawFloatBesideAStringConstantIsTheDesignedBoxExitAndBesideAVariableAGuard) {
  GLTANG_REQUIRE_JIT_BACKEND();
  // `"x" + f` with f raw is a concatenation, which converts the float to text: the designed box
  // exit, not counted, and the interpreter makes the text. A string from a variable beside a raw
  // float is float arithmetic in compiled code, whose unbox of the string is a real guard exit.
  // An index or a slice by a raw float is the designed box exit too. The loop keeps each function
  // from the static decline.
  struct Case {
    const char * name;
    const char * body;
    uint64_t expected, counted;
  };
  const Case cases[] = {
      {"a string constant on the left", "x = 1.5 * 2.0; return \"x\" + x;", 2, 0},
      {"a string constant on the right", "x = 1.5 * 2.0; return x + \"x\";", 2, 0},
      {"a string from a variable", "t = \"x\"; x = 1.5 * 2.0; return t + x;", 0, 2},
      {"an index by a raw float", "t = \"abc\"; x = 0.5 * 2.0; return t[x];", 2, 0},
      {"a slice by a raw float", "t = \"abc\"; x = 0.5 * 2.0; return t[x:];", 2, 0},
  };
  for (const Case & c : cases) {
    Scenario sc = scenario(std::string("function f() { i = 0; while (i < 2) { i = i + 1; } ") + c.body + " }\nprint(f()); print(\";\"); print(f());\n");
    Outcome plain, jit;
    expect_same(sc, &plain, &jit);
    ASSERT_FALSE(HasFailure()) << c.name;
    EXPECT_EQ(jit.raw, plain.raw) << c.name;
    EXPECT_EQ(jit.stats.exits_expected, c.expected) << c.name;
    EXPECT_EQ(jit.stats.exits_counted, c.counted) << c.name;
    EXPECT_EQ(jit.stats.functions_declined_expected, 0u) << c.name;
  }
}

TEST(JitTemplates, WhereNativesCannotBeCalledTheValueOperationsStayExitsAndTheOutputIsTheSame) {
  GLTANG_REQUIRE_JIT_BACKEND();
  Scenario sc = scenario("function f(s, n) { r = 0; i = 0; while (i < n) { r = r + (s + s)[1:3].length + s[0].length; i = i + 1; } return r; }\nx = f(\"ab\", 3); x;\n");
  sc.natives_off = true;
  Outcome plain, jit;
  expect_same(sc, &plain, &jit);
  ASSERT_FALSE(HasFailure());
  EXPECT_EQ(jit.result, plain.result);
  EXPECT_EQ(jit.stats.value_calls, 0u);
  EXPECT_GE(jit.stats.deopts, 1u) << "a value operation left compiled code";
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
  EXPECT_EQ(st.output_calls, 0u);
  EXPECT_EQ(st.result_clears, 0u);
  EXPECT_EQ(st.functions_compiled, 0u);
  // A template, whose text clears its result: still nothing compiled or counted.
  Compiled text("head<% if (1 < 2) { %>yes<% } %>tail", Mode::Template, "nojit-template.tang");
  ASSERT_TRUE(text.ok());
  Context tcontext(text.program);
  ASSERT_TRUE(tcontext.ok());
  ASSERT_TRUE(tcontext.execute());
  EXPECT_EQ(tcontext.raw(), "headyestail");
  GLTANG_JitStats tst = tcontext.jit_stats();
  EXPECT_EQ(tst.output_calls, 0u);
  EXPECT_EQ(tst.result_clears, 0u);
  EXPECT_EQ(tst.functions_compiled, 0u);
}

#endif

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
