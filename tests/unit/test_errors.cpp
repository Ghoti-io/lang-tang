// The error list (CAP-1; AD-13): which errors enter it, what an entry says, the
// host's switch that logs every error at creation, the cap, and the option that
// ends the run at the first error.

#include "exec_harness.h"

using tt::Compiled;
using tt::Config;
using tt::Context;

namespace {

// Compiles `source` as `file`, names the main program `name`, and runs it.
struct Named {
  Compiled compiled;
  Context context;
  explicit Named(const std::string & source, const char * name = "page", bool log_all = false, bool halt = false, tt::Mode mode = tt::Mode::Script)
    : compiled(source, mode), context(compiled.program) {
    EXPECT_TRUE(compiled.ok()) << compiled.error.message;
    EXPECT_TRUE(context.ok());
    EXPECT_EQ(gltang_execution_set_name(context.execution, name), GLTANG_OK);
    if (log_all) {
      EXPECT_EQ(gltang_execution_set_log_all_errors(context.execution, true), GLTANG_OK);
    }
    if (halt) {
      EXPECT_EQ(gltang_execution_set_halt_on_error(context.execution, true), GLTANG_OK);
    }
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// Swallowed: printed
// ---------------------------------------------------------------------------

TEST(ErrorList, APrintedErrorIsListedWithItsTemplateAndOrigin) {
  Named run("x = 1 / 0;\nprint(x);\nprint(\"after\");", "page");
  ASSERT_TRUE(run.context.execute());
  EXPECT_EQ(run.context.raw(), "after");
  ASSERT_EQ(run.context.error_count(), 1u);
  auto e = run.context.error(0);
  EXPECT_EQ(e.e.kind, GLTANG_ERROR_DIVIDE_BY_ZERO);
  EXPECT_EQ(e.message(), "Divide by zero");
  EXPECT_EQ(e.e.how, GLTANG_ERROR_HOW_PRINTED);
  EXPECT_EQ(e.template_name(), "page");
  EXPECT_EQ(e.chain.size(), 0u);
  EXPECT_EQ(e.file(), "test.tang");
  EXPECT_EQ(e.e.line, 1) << "the line of the division, not of the print";
}

TEST(ErrorList, AnErrorThatIsStoredAndNeverPrintedIsNotListed) {
  Named run("y = 1 / 0; z = y; print(\"after\");");
  ASSERT_TRUE(run.context.execute());
  EXPECT_EQ(run.context.error_count(), 0u);
}

TEST(ErrorList, AnErrorTestedAndHandledIsNotListed) {
  Named run("e = 1 / 0; if (e) { print(\"error\"); } else { print(\"handled\"); } e; r = e == null; print(r); e;");
  ASSERT_TRUE(run.context.execute());
  EXPECT_EQ(run.context.error_count(), 0u) << run.context.error(0).message();
}

TEST(ErrorList, AMarkerPrintsItselfAndIsNotListed) {
  Named run("print(9223372036854775807 + 1);");
  ASSERT_TRUE(run.context.execute());
  EXPECT_EQ(run.context.raw(), "[INTEGER TOO LARGE]");
  EXPECT_EQ(run.context.error_count(), 0u);
}

TEST(ErrorList, AnErrorPrintedTwiceIsListedOnce) {
  Named run("x = 1 / 0; print(x); print(x);");
  ASSERT_TRUE(run.context.execute());
  EXPECT_EQ(run.context.error_count(), 1u);
}

TEST(ErrorList, EachErrorValueIsItsOwnEntryAndKeepsItsOwnLine) {
  Named run("print(1 / 0);\nprint(1 % 0);\nprint(\"a\" - 1);\n");
  ASSERT_TRUE(run.context.execute());
  ASSERT_EQ(run.context.error_count(), 3u);
  EXPECT_EQ(run.context.error(0).e.kind, GLTANG_ERROR_DIVIDE_BY_ZERO);
  EXPECT_EQ(run.context.error(0).e.line, 1);
  EXPECT_EQ(run.context.error(1).e.kind, GLTANG_ERROR_MODULO_BY_ZERO);
  EXPECT_EQ(run.context.error(1).e.line, 2);
  EXPECT_EQ(run.context.error(2).e.kind, GLTANG_ERROR_NOT_SUPPORTED);
  EXPECT_EQ(run.context.error(2).e.line, 3);
}

TEST(ErrorList, ATemplateWithABrokenExpressionRunsToTheEndAndNamesTheError) {
  Named run("<p><%= 1 / 0 %></p>\n<%= 2 %>\n", "page", false, false, tt::Mode::Template);
  ASSERT_TRUE(run.context.execute());
  EXPECT_EQ(run.context.raw(), "<p></p>\n2\n");
  ASSERT_EQ(run.context.error_count(), 1u);
  auto e = run.context.error(0);
  EXPECT_EQ(e.e.how, GLTANG_ERROR_HOW_PRINTED);
  EXPECT_EQ(e.e.line, 1);
  EXPECT_EQ(e.template_name(), "page");
}

// ---------------------------------------------------------------------------
// Swallowed: discarded
// ---------------------------------------------------------------------------

TEST(ErrorList, ADiscardedStringStoreIsListed) {
  // Reference 13.9: ctang discards this error; lang-tang lists it.
  Named run("s = \"abc\";\ns[0] = \"x\";\nprint(s);");
  ASSERT_TRUE(run.context.execute());
  EXPECT_EQ(run.context.raw(), "abc");
  ASSERT_EQ(run.context.error_count(), 1u);
  auto e = run.context.error(0);
  EXPECT_EQ(e.e.kind, GLTANG_ERROR_NOT_SUPPORTED);
  EXPECT_EQ(e.e.how, GLTANG_ERROR_HOW_DISCARDED);
  EXPECT_EQ(e.e.line, 2);
}

TEST(ErrorList, ADroppedErrorFollowedByALoopOrAnIfIsListed) {
  // CLEAR_RESULT, not SET_RESULT, ends the next statement: it must list too.
  Named run("1 / 0;\nwhile (false) {}\nprint(\"x\");");
  ASSERT_TRUE(run.context.execute());
  ASSERT_EQ(run.context.error_count(), 1u);
  EXPECT_EQ(run.context.error(0).e.how, GLTANG_ERROR_HOW_DISCARDED);
  EXPECT_EQ(run.context.error(0).e.line, 1);
}

TEST(ErrorList, ADroppedErrorFollowedByTemplateTextIsListed) {
  Named run("<% 1 / 0; %>text\n", "page", false, false, tt::Mode::Template);
  ASSERT_TRUE(run.context.execute());
  EXPECT_EQ(run.context.raw(), "text\n");
  ASSERT_EQ(run.context.error_count(), 1u);
  EXPECT_EQ(run.context.error(0).e.how, GLTANG_ERROR_HOW_DISCARDED);
}

TEST(ErrorList, ALoopOverSomethingNotIterableIsListedWhenItIsFollowedByAStatement) {
  Named run("for (x : \"abc\") {}\nprint(1);");
  ASSERT_TRUE(run.context.execute());
  ASSERT_EQ(run.context.error_count(), 1u);
  EXPECT_EQ(run.context.error(0).e.kind, GLTANG_ERROR_NOT_IMPLEMENTED);
  EXPECT_EQ(run.context.error(0).e.how, GLTANG_ERROR_HOW_DISCARDED);
  Named number("for (x : 3) {}\nprint(1);");
  ASSERT_TRUE(number.context.execute());
  ASSERT_EQ(number.context.error_count(), 1u);
  EXPECT_EQ(number.context.error(0).e.kind, GLTANG_ERROR_NOT_SUPPORTED);
}

TEST(ErrorList, ABadLoopInsideAFunctionDoesNotListAgainstTheCallersStatement) {
  Named run("function f() { for (x : 3) {} return 1; }\nf();\nprint(2);");
  ASSERT_TRUE(run.context.execute());
  EXPECT_LE(run.context.error_count(), 1u);
  for (size_t i = 0; i < run.context.error_count(); ++i) {
    EXPECT_EQ(run.context.error(i).e.kind, GLTANG_ERROR_NOT_SUPPORTED);
  }
}

TEST(ErrorList, TheLastStatementOfTheTopLevelIsTheResultAndIsNotListed) {
  Named run("s = \"abc\";\ns[0] = \"x\";");
  ASSERT_TRUE(run.context.execute());
  EXPECT_EQ(run.context.error_count(), 0u);
  EXPECT_TRUE(run.context.is_error());
}

TEST(ErrorList, AStatementThatIsAnErrorAndIsFollowedByAnotherIsDiscarded) {
  Named run("1 / 0;\nprint(\"x\");");
  ASSERT_TRUE(run.context.execute());
  ASSERT_EQ(run.context.error_count(), 1u);
  EXPECT_EQ(run.context.error(0).e.how, GLTANG_ERROR_HOW_DISCARDED);
  EXPECT_EQ(run.context.error(0).e.line, 1);
}

TEST(ErrorList, AnErrorInsideALoopIsListedForEveryIteration) {
  Named run("s = \"abc\";\nfor (i = 0; i < 3; i += 1) {\n  s[0] = \"x\";\n}\nprint(\"done\");");
  ASSERT_TRUE(run.context.execute());
  EXPECT_EQ(run.context.error_count(), 3u);
  EXPECT_EQ(run.context.error(2).e.line, 3);
}

TEST(ErrorList, ACallWhoseValueIsAnErrorAndIsDroppedInsideAFunctionIsListed) {
  Named run("function f() {\n  1 / 0;\n  s = \"abc\";\n  s[0] = \"x\";\n  return 1;\n}\nf();\nprint(f());", "main");
  ASSERT_TRUE(run.context.execute());
  EXPECT_EQ(run.context.raw(), "1");
  ASSERT_EQ(run.context.error_count(), 4u) << "two statements, in each of two calls";
  EXPECT_EQ(run.context.error(0).e.line, 2);
  EXPECT_EQ(run.context.error(1).e.line, 4);
  EXPECT_EQ(run.context.error(0).e.how, GLTANG_ERROR_HOW_DISCARDED);
}

TEST(ErrorList, AnErrorFromACallDroppedAtTheTopLevelIsListedWhenReplaced) {
  Named run("function f() { return 1 / 0; }\nf();\n1;");
  ASSERT_TRUE(run.context.execute());
  ASSERT_EQ(run.context.error_count(), 1u);
  EXPECT_EQ(run.context.error(0).e.line, 1);
}

// ---------------------------------------------------------------------------
// The switch, the cap, and memory
// ---------------------------------------------------------------------------

TEST(ErrorList, TheLogAllSwitchEntersEveryErrorAtCreationAndOnlyOnce) {
  Named run("x = 1 / 0;\ny = 2 % 0;\nprint(x);\nz = x;", "page", true);
  ASSERT_TRUE(run.context.execute());
  ASSERT_EQ(run.context.error_count(), 2u) << "created, not created and printed";
  EXPECT_EQ(run.context.error(0).e.how, GLTANG_ERROR_HOW_CREATED);
  EXPECT_EQ(run.context.error(0).e.line, 1);
  EXPECT_EQ(run.context.error(1).e.how, GLTANG_ERROR_HOW_CREATED);
  EXPECT_EQ(run.context.error(1).e.line, 2);
}

TEST(ErrorList, AMarkerIsEnteredAtCreationUnderLogAllAndNotWhenPrinted) {
  {
    Named run("print(9223372036854775807 + 1); print(\"x\");", "page", true);
    ASSERT_TRUE(run.context.execute());
    ASSERT_EQ(run.context.error_count(), 1u);
    EXPECT_EQ(run.context.error(0).e.kind, GLTANG_ERROR_INTEGER_TOO_LARGE);
    EXPECT_EQ(run.context.error(0).e.how, GLTANG_ERROR_HOW_CREATED);
  }
  {
    Named run("print(9223372036854775807 + 1);");
    ASSERT_TRUE(run.context.execute());
    EXPECT_EQ(run.context.error_count(), 0u);
  }
}

TEST(ErrorList, ASwallowedCaseIsNotDoubledUnderLogAll) {
  Named run("s = \"abc\";\ns[0] = \"x\";\nprint(s);\nprint(1 / 0);", "page", true);
  ASSERT_TRUE(run.context.execute());
  EXPECT_EQ(run.context.error_count(), 2u);
}

TEST(ErrorList, TheDefaultCapIs1024AndTheRestAreCounted) {
  Named run("for (i = 0; i < 2000; i += 1) { print(1 / 0); }");
  ASSERT_TRUE(run.context.execute());
  EXPECT_EQ(run.context.error_count(), 1024u);
  EXPECT_EQ(gltang_execution_errors_dropped(run.context.execution), 976u);
}

TEST(ErrorList, TheCapIsTheHostsToSetAndZeroKeepsNothing) {
  {
    Named run("for (i = 0; i < 10; i += 1) { print(1 / 0); }");
    ASSERT_EQ(gltang_execution_set_error_limit(run.context.execution, 4), GLTANG_OK);
    ASSERT_TRUE(run.context.execute());
    EXPECT_EQ(run.context.error_count(), 4u);
    EXPECT_EQ(gltang_execution_errors_dropped(run.context.execution), 6u);
  }
  {
    Named run("print(1 / 0);");
    ASSERT_EQ(gltang_execution_set_error_limit(run.context.execution, 0), GLTANG_OK);
    ASSERT_TRUE(run.context.execute());
    EXPECT_EQ(run.context.error_count(), 0u);
    EXPECT_EQ(gltang_execution_errors_dropped(run.context.execution), 1u);
  }
}

TEST(ErrorList, TheEntriesAreChargedToTheContextsAllocator) {
  Compiled compiled("for (i = 0; i < 200; i += 1) { print(1 / 0); }");
  Compiled quiet("for (i = 0; i < 200; i += 1) { print(1); }");
  Context with(compiled.program);
  Context without(quiet.program);
  ASSERT_TRUE(with.execute());
  ASSERT_TRUE(without.execute());
  EXPECT_EQ(with.error_count(), 200u);
  // The list is 200 entries of the context's allocator, not the process's.
  EXPECT_GT(grcore_context_memory_in_use(with.context), grcore_context_memory_in_use(without.context) + 200u * sizeof(GLTANG_ErrorEntry));
}

TEST(ErrorList, TheAccessorsAnswerForBadIndexesAndForNull) {
  Named run("print(1 / 0);");
  ASSERT_TRUE(run.context.execute());
  GLTANG_ErrorEntry entry;
  GLTANG_ErrorLink link;
  EXPECT_FALSE(gltang_execution_error(run.context.execution, 1, &entry));
  EXPECT_FALSE(gltang_execution_error(run.context.execution, 0, nullptr));
  EXPECT_FALSE(gltang_execution_error(nullptr, 0, &entry));
  EXPECT_EQ(gltang_execution_error_count(nullptr), 0u);
  EXPECT_EQ(gltang_execution_error_chain_count(run.context.execution, 5), 0u);
  EXPECT_FALSE(gltang_execution_error_chain(run.context.execution, 0, 0, &link));
  EXPECT_EQ(gltang_execution_errors_dropped(nullptr), 0u);
  gltang_execution_destroy(run.context.execution);
  EXPECT_EQ(gltang_execution_error_count(run.context.execution), 0u) << "after a destroy the list is gone with the rest";
}

TEST(ErrorList, TheMainProgramIsNamedByTheHostThenByItsFileThenMain) {
  {
    Named run("print(1 / 0);", "page");
    ASSERT_TRUE(run.context.execute());
    EXPECT_EQ(run.context.error(0).template_name(), "page");
  }
  {
    Compiled compiled("print(1 / 0);", tt::Mode::Script, "home.tang");
    Context context(compiled.program);
    ASSERT_TRUE(context.execute());
    EXPECT_EQ(context.error(0).template_name(), "home.tang");
  }
  {
    Compiled compiled("print(1 / 0);", tt::Mode::Script, nullptr);
    Context context(compiled.program);
    ASSERT_TRUE(context.execute());
    EXPECT_EQ(context.error(0).template_name(), "main");
  }
}

// ---------------------------------------------------------------------------
// Halt on the first error
// ---------------------------------------------------------------------------

TEST(Halt, TheFirstErrorEndsTheRunWithErrGuest) {
  Named run("print(\"a\"); x = 1 / 0; print(\"b\");", "page", false, true);
  EXPECT_FALSE(run.context.execute());
  EXPECT_EQ(run.context.ran, GRCORE_ERR_GUEST);
  EXPECT_EQ(grcore_context_unwind_result(run.context.context), GRCORE_ERR_GUEST);
  EXPECT_EQ(gltang_execution_state(run.context.execution), GLTANG_EXECUTION_UNWOUND);
  EXPECT_EQ(run.context.raw(), "a");
  EXPECT_EQ(grcore_stack_frame_count(grcore_context_stack(run.context.context)), 0u);
  ASSERT_EQ(run.context.error_count(), 1u);
  EXPECT_EQ(run.context.error(0).e.how, GLTANG_ERROR_HOW_CREATED);
  EXPECT_EQ(run.context.error(0).e.kind, GLTANG_ERROR_DIVIDE_BY_ZERO);
  EXPECT_TRUE(run.context.is_null()) << "an unwound run has no result";
}

TEST(Halt, OffTheRunFinishes) {
  Named run("print(\"a\"); x = 1 / 0; print(\"b\");");
  EXPECT_TRUE(run.context.execute());
  EXPECT_EQ(run.context.raw(), "ab");
}

TEST(Halt, ItEndsARunFromDeepInACallAndDoesNotNeedTheErrorToBePrinted) {
  Named run("function f(n) { if (n == 0) { return 1 / 0; } return f(n - 1); }\nprint(\"a\");\nf(30);\nprint(\"b\");", "page", false, true);
  EXPECT_FALSE(run.context.execute());
  EXPECT_EQ(run.context.ran, GRCORE_ERR_GUEST);
  EXPECT_EQ(run.context.raw(), "a");
  EXPECT_GE(gltang_execution_unwound_frames(run.context.execution), 31u);
}

TEST(Halt, AMarkerIsAnErrorValueToo) {
  Named run("print(\"a\"); print(9223372036854775807 + 1); print(\"b\");", "page", false, true);
  EXPECT_FALSE(run.context.execute());
  EXPECT_EQ(run.context.raw(), "a");
  EXPECT_EQ(run.context.error_count(), 1u);
}

TEST(Halt, AnErrorInsideANativeOperationEndsTheRunToo) {
  Named run("print(\"a\"); x = \"abc\"[0:1:0]; print(\"b\");", "page", false, true);
  EXPECT_FALSE(run.context.execute());
  EXPECT_EQ(run.context.raw(), "a");
}

// ---------------------------------------------------------------------------
// Halt on an error that no built-in operation made
// ---------------------------------------------------------------------------

namespace {

bool host_refuses(GLTANG_NativeCall *, void *) { return false; }

bool host_errors(GLTANG_NativeCall * call, void *) {
  gltang_call_return_error(call, GLTANG_ERROR_INVALID_FUNCTION_CALL);
  return true;
}

bool host_works(GLTANG_NativeCall * call, void *) {
  gltang_call_return_integer(call, 5);
  return true;
}

/// Runs `source` with the natives `refuses`, `errors` and `works` and the
/// template `sidebar` (`print("<aside>")`) in the context's library, halting
/// on the first error.
struct HaltHost {
  Compiled compiled;
  Compiled sidebar;
  Context context;
  HaltHost(const std::string & source, bool halt, long jit_threshold = 0)
    : compiled(source, tt::Mode::Script), sidebar("print(\"<aside>\");", tt::Mode::Script),
      context(compiled.program, [&] { Config c; c.jit_threshold = jit_threshold; return c; }()) {
    EXPECT_TRUE(compiled.ok()) << compiled.error.message;
    EXPECT_TRUE(sidebar.ok());
    EXPECT_TRUE(context.ok());
    EXPECT_EQ(gltang_library_add_native(context.library(), "refuses", host_refuses, nullptr), GLTANG_OK);
    EXPECT_EQ(gltang_library_add_native(context.library(), "errors", host_errors, nullptr), GLTANG_OK);
    EXPECT_EQ(gltang_library_add_native(context.library(), "works", host_works, nullptr), GLTANG_OK);
    EXPECT_EQ(gltang_library_add_template(context.library(), "sidebar", sidebar.program, 100000, GLTANG_SCOPE_EMPTY), GLTANG_OK);
    if (halt) {
      EXPECT_EQ(gltang_execution_set_halt_on_error(context.execution, true), GLTANG_OK);
    }
  }
};

void expect_halted(HaltHost & host, GLTANG_ErrorKind kind, const char * printed_before) {
  EXPECT_FALSE(host.context.execute());
  EXPECT_EQ(host.context.ran, GRCORE_ERR_GUEST);
  EXPECT_EQ(gltang_execution_state(host.context.execution), GLTANG_EXECUTION_UNWOUND);
  EXPECT_EQ(host.context.raw(), printed_before);
  ASSERT_EQ(host.context.error_count(), 1u);
  EXPECT_EQ(host.context.error(0).e.kind, kind);
  EXPECT_EQ(host.context.error(0).e.how, GLTANG_ERROR_HOW_CREATED);
  EXPECT_EQ(grcore_stack_frame_count(grcore_context_stack(host.context.context)), 0u);
}

}  // namespace

TEST(Halt, AHostFunctionThatFailsEndsTheRun) {
  HaltHost host("use refuses; print(\"a\"); x = refuses(1); print(\"b\");", true);
  expect_halted(host, GLTANG_ERROR_HOST_FAILED, "a");
}

TEST(Halt, AHostFunctionThatAnswersWithAnErrorEndsTheRun) {
  HaltHost host("use errors; print(\"a\"); x = errors(); print(\"b\");", true);
  expect_halted(host, GLTANG_ERROR_INVALID_FUNCTION_CALL, "a");
}

TEST(Halt, ATemplateCalledWithAnArgumentEndsTheRun) {
  HaltHost host("use sidebar; print(\"a\"); x = sidebar(1); print(\"b\");", true);
  expect_halted(host, GLTANG_ERROR_ARGUMENT_COUNT_MISMATCH, "a");
}

TEST(Halt, TheSameCallsWithHaltOffAreErrorValuesAndTheRunFinishes) {
  // The control: the three errors above are made without the option and the
  // run goes on, so it is the option that ends the others.
  HaltHost host(
      "use refuses; use errors; use sidebar; use works;\n"
      "a = refuses(1); b = errors(); c = sidebar(1); print(\"done\"); works();",
      false);
  EXPECT_TRUE(host.context.execute());
  EXPECT_EQ(host.context.raw(), "done");
}

TEST(Halt, TheHostAndTemplateErrorsEndARunInCompiledCodeToo) {
  for (const char * source : {"use refuses; function f(n) { return refuses(n); } print(\"a\"); f(1); print(\"b\");",
           "use sidebar; function f(n) { return sidebar(n); } print(\"a\"); f(1); print(\"b\");"}) {
    HaltHost host(source, true, 1);
    EXPECT_FALSE(host.context.execute());
    EXPECT_EQ(host.context.ran, GRCORE_ERR_GUEST);
    EXPECT_EQ(host.context.raw(), "a");
    EXPECT_EQ(host.context.error_count(), 1u);
  }
}


TEST(Halt, WithBothSwitchesTheErrorIsEnteredOnce) {
  Named run("print(\"a\"); x = 1 / 0; print(\"b\");", "page", true, true);
  EXPECT_FALSE(run.context.execute());
  EXPECT_EQ(run.context.error_count(), 1u);
}

TEST(Halt, TurningItOffAgainBeforeTheRunLeavesAPlainRun) {
  Named run("print(\"a\"); x = 1 / 0; print(\"b\");", "page", false, true);
  ASSERT_EQ(gltang_execution_set_halt_on_error(run.context.execution, false), GLTANG_OK);
  EXPECT_TRUE(run.context.execute());
  EXPECT_EQ(run.context.raw(), "ab");
}

TEST(Halt, TurningItOnTwiceIsHarmless) {
  Named run("print(\"a\"); x = 1 / 0; print(\"b\");", "page", false, true);
  ASSERT_EQ(gltang_execution_set_halt_on_error(run.context.execution, true), GLTANG_OK);
  EXPECT_FALSE(run.context.execute());
  EXPECT_EQ(run.context.ran, GRCORE_ERR_GUEST);
}

TEST(Halt, TheSettersAreRefusedOnceTheRunHasStarted) {
  Named run("1;");
  ASSERT_TRUE(run.context.execute());
  EXPECT_EQ(gltang_execution_set_halt_on_error(run.context.execution, true), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_execution_set_log_all_errors(run.context.execution, true), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_execution_set_error_limit(run.context.execution, 3), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_execution_set_halt_on_error(nullptr, true), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_execution_set_log_all_errors(nullptr, true), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_execution_set_error_limit(nullptr, 3), GLTANG_ERR_INVALID);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
