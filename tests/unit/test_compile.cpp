/**
 * @file
 *
 * The compiler's own contract: what it refuses and where it says so, what a
 * program lets a host ask of it, the tree-depth budget, and allocation failure
 * during compile and during a run.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "exec_harness.h"
#include "test_helpers.h"

#include <unistd.h>

#include <cstdio>
#include <vector>
#include <string>

namespace {

std::string chain(int pluses) {
  std::string s = "1";
  for (int i = 0; i < pluses; ++i) {
    s += "+1";
  }
  return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// What the compiler refuses
// ---------------------------------------------------------------------------

struct Refusal {
  const char * source;
  int line;
  int column;
  const char * message;
};

TEST(CompileErrors, EachRefusalNamesItsLineAndColumn) {
  const Refusal rows[] = {
    {"x = 1;\nf() = 2;", 2, 1, "Cannot assign to this expression."},
    {"1 + 2 = 3;", 1, 1, "Cannot assign to this expression."},
    {"function f(a, a) { }", 1, 1, "Parameter 'a' is declared twice."},
    {"global y;", 1, 1, "A global declaration is only permitted inside a function."},
    {"x = 1; function x() {}", 1, 8, "'x' is already declared in this scope."},
    // Inside a function (ctang refuses each of these too).
    {"function g() { function h() {} h = 1; }", 1, 32, "'h' is already declared in this scope."},
    {"function g() { x = 1; global x; }", 1, 23, "'x' is already declared in this scope."},
    {"function g() { function h() {} function h() {} }", 1, 32, "'h' is already declared in this scope."},
    {"function a() {} function g() { a = 1; }", 1, 32, "'a' is already declared in this scope."},
    {"function f() { return 1; } f = 2;", 1, 28, "'f' is already declared in this scope."},
  };
  for (const Refusal & row : rows) {
    tt::Compiled compiled(row.source);
    EXPECT_EQ(compiled.result, GLTANG_ERR_FORMAT) << row.source;
    EXPECT_EQ(compiled.program, nullptr) << row.source;
    EXPECT_EQ(compiled.error.line, row.line) << row.source;
    EXPECT_EQ(compiled.error.column, row.column) << row.source;
    EXPECT_STREQ(compiled.error.message, row.message) << row.source;
  }
}

TEST(CompileErrors, AnErrorIsWrittenOnlyOnARefusal) {
  GLTANG_Tree * tree = nullptr;
  GLTANG_ParseError error = {0, 0, {0}};
  ASSERT_EQ(gltang_parse("1;", GLTANG_PARSE_SCRIPT, &error, &tree), GLTANG_OK);
  GLTANG_Program * program = nullptr;
  GLTANG_ParseError mine = {7, 8, "keep"};
  EXPECT_EQ(gltang_compile(tree, "t.tang", &mine, &program), GLTANG_OK);
  EXPECT_EQ(mine.line, 7);
  EXPECT_STREQ(mine.message, "keep");
  gltang_program_release(program);
  EXPECT_EQ(gltang_compile(tree, "t.tang", nullptr, &program), GLTANG_OK) << "the error out is optional";
  gltang_program_release(program);
  gltang_tree_destroy(tree);
}

TEST(CompileErrors, InvalidArgumentsAreRefusedAndWriteNothing) {
  GLTANG_Tree * tree = nullptr;
  GLTANG_ParseError error = {0, 0, {0}};
  ASSERT_EQ(gltang_parse("1;", GLTANG_PARSE_SCRIPT, &error, &tree), GLTANG_OK);
  GLTANG_Program * sentinel = reinterpret_cast<GLTANG_Program *>(0x3);
  GLTANG_Program * program = sentinel;
  EXPECT_EQ(gltang_compile(nullptr, "t", &error, &program), GLTANG_ERR_INVALID);
  EXPECT_EQ(program, sentinel);
  EXPECT_EQ(gltang_compile(tree, "t", &error, nullptr), GLTANG_ERR_INVALID);
  gltang_tree_destroy(tree);
}

TEST(CompileErrors, ARefusalLeavesNothingAllocated) {
  alloc_sweep::track(true);
  {
    tt::Compiled compiled("function f(a, a) { }");
    EXPECT_EQ(compiled.result, GLTANG_ERR_FORMAT);
  }
  EXPECT_EQ(alloc_sweep::live(), 0u);
  alloc_sweep::track(false);
}

// ---------------------------------------------------------------------------
// What a program tells a host
// ---------------------------------------------------------------------------

TEST(Program, ItNamesItsFileItsFunctionsAndTheirSizes) {
  tt::Compiled compiled("function a() { return 1; }\nfunction b(x, y) { return x + y; }\na() + b(1, 2);", tt::Mode::Script, "p.tang");
  ASSERT_TRUE(compiled.ok());
  const GLTANG_Program * p = compiled.program;
  EXPECT_STREQ(gltang_program_file(p), "p.tang");
  ASSERT_EQ(gltang_program_function_count(p), 3u) << "the top level, a, b";
  EXPECT_STREQ(gltang_program_function_name(p, 1), "a");
  EXPECT_STREQ(gltang_program_function_name(p, 2), "b");
  EXPECT_NE(gltang_program_function_name(p, 0), nullptr) << "the top level has a name too";
  for (size_t f = 0; f < 3; ++f) {
    EXPECT_GT(gltang_program_function_size(p, f), 0u);
    EXPECT_GT(gltang_program_function_max_stack(p, f), 0u);
  }
  EXPECT_EQ(gltang_program_function_name(p, 3), nullptr);
  EXPECT_EQ(gltang_program_function_size(p, 3), 0u);
  EXPECT_EQ(gltang_program_function_max_stack(p, 3), 0u);
}

TEST(Program, NullsAnswerAsEmpty) {
  EXPECT_EQ(gltang_program_file(nullptr), nullptr);
  EXPECT_EQ(gltang_program_function_count(nullptr), 0u);
  EXPECT_EQ(gltang_program_function_name(nullptr, 0), nullptr);
  int line = 5;
  EXPECT_NE(gltang_program_locate(nullptr, 0, 0, &line), GLTANG_OK);
  gltang_program_dump(nullptr, stdout);  // prints nothing, does not crash
}

TEST(Program, AnEmptySourceCompilesToAProgramThatDoesNothing) {
  tt::Compiled compiled("");
  ASSERT_TRUE(compiled.ok());
  EXPECT_EQ(gltang_program_function_count(compiled.program), 1u);
  tt::Context context(compiled.program);
  ASSERT_TRUE(context.execute());
  EXPECT_TRUE(context.is_null());
}

TEST(Program, LocateGivesLinesAndRefusesPlacesThatAreNotInTheProgram) {
  tt::Compiled compiled("x = 1;\n\ny = 2;\n");
  ASSERT_TRUE(compiled.ok());
  int line = 0;
  int highest = 0;
  for (uint64_t offset = 0; offset < gltang_program_function_size(compiled.program, 0); ++offset) {
    ASSERT_EQ(gltang_program_locate(compiled.program, 0, offset, &line), GLTANG_OK);
    EXPECT_GE(line, 1);
    highest = std::max(highest, line);
  }
  EXPECT_EQ(highest, 3);
  EXPECT_NE(gltang_program_locate(compiled.program, 9, 0, &line), GLTANG_OK);
  EXPECT_NE(gltang_program_locate(compiled.program, 0, 100000, &line), GLTANG_OK);
}

TEST(Program, ADumpListsEveryFunctionWithOpcodeNames) {
  tt::Compiled compiled("function f(a) { return a + 1; }\nf(2);");
  ASSERT_TRUE(compiled.ok());
  FILE * file = tmpfile();
  ASSERT_NE(file, nullptr);
  gltang_program_dump(compiled.program, file);
  long length = ftell(file);
  ASSERT_GT(length, 0);
  rewind(file);
  std::string text(length, '\0');
  ASSERT_EQ(fread(&text[0], 1, length, file), (size_t)length);
  fclose(file);
  EXPECT_NE(text.find("POLL"), std::string::npos);
  EXPECT_NE(text.find("ADD"), std::string::npos);
  EXPECT_NE(text.find("CALL"), std::string::npos);
  EXPECT_NE(text.find("RET"), std::string::npos);
}

TEST(Program, RetainAndReleaseCountReferencesAndSurviveTheTree) {
  GLTANG_Tree * tree = nullptr;
  GLTANG_ParseError error = {0, 0, {0}};
  ASSERT_EQ(gltang_parse("1 + 2;", GLTANG_PARSE_SCRIPT, &error, &tree), GLTANG_OK);
  GLTANG_Program * program = nullptr;
  ASSERT_EQ(gltang_compile(tree, "t", &error, &program), GLTANG_OK);
  gltang_tree_destroy(tree);  // the program does not point into the tree
  GLTANG_Program * again = gltang_program_retain(program);
  EXPECT_EQ(again, program);
  gltang_program_release(program);
  tt::Context context(again);
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(context.integer(), 3);
  gltang_program_release(again);
}

// ---------------------------------------------------------------------------
// The tree-depth budget
// ---------------------------------------------------------------------------

TEST(TreeDepth, ATreeAtTheBudgetParsesCompilesAndRuns) {
  tt::Run run(chain(GLTANG_MAX_TREE_DEPTH - 1));
  ASSERT_TRUE(run.context.is_integer());
  EXPECT_EQ(run.context.integer(), (int64_t)GLTANG_MAX_TREE_DEPTH);
}

TEST(TreeDepth, ATreeOneOverIsRefusedByTheParserWithTheLimitResult) {
  std::string source = chain(GLTANG_MAX_TREE_DEPTH);
  GLTANG_ParseError error = {0, 0, {0}};
  GLTANG_Tree * tree = reinterpret_cast<GLTANG_Tree *>(0x3);
  EXPECT_EQ(gltang_parse(source.c_str(), GLTANG_PARSE_SCRIPT, &error, &tree), GLTANG_ERR_LIMIT);
  EXPECT_EQ(tree, reinterpret_cast<GLTANG_Tree *>(0x3)) << "a refusal builds no tree";
}

TEST(TreeDepth, TheRefusalIsPerParseAndTheNextParseIsNotPoisoned) {
  std::string tall = chain(GLTANG_MAX_TREE_DEPTH);
  GLTANG_ParseError error = {0, 0, {0}};
  GLTANG_Tree * tree = nullptr;
  EXPECT_EQ(gltang_parse(tall.c_str(), GLTANG_PARSE_SCRIPT, &error, &tree), GLTANG_ERR_LIMIT);
  EXPECT_EQ(gltang_parse("1 + 2;", GLTANG_PARSE_SCRIPT, &error, &tree), GLTANG_OK);
  gltang_tree_destroy(tree);
}

TEST(TreeDepth, TheRefusalLeaksNothing) {
  std::string tall = chain(GLTANG_MAX_TREE_DEPTH);
  alloc_sweep::track(true);
  GLTANG_ParseError error = {0, 0, {0}};
  GLTANG_Tree * tree = nullptr;
  EXPECT_EQ(gltang_parse(tall.c_str(), GLTANG_PARSE_SCRIPT, &error, &tree), GLTANG_ERR_LIMIT);
  EXPECT_EQ(alloc_sweep::live(), 0u);
  alloc_sweep::track(false);
}

TEST(TreeDepth, DeeplyNestedBlocksHitTheParsersOwnNestingLimit) {
  // Blocks nest in the grammar's stack before they can make the tree too tall,
  // so this is the parser's limit (the same figure) and the same result.
  std::string source;
  for (int i = 0; i < 20000; ++i) {
    source += "{";
  }
  source += "1;";
  for (int i = 0; i < 20000; ++i) {
    source += "}";
  }
  GLTANG_ParseError error = {0, 0, {0}};
  GLTANG_Tree * tree = nullptr;
  EXPECT_EQ(gltang_parse(source.c_str(), GLTANG_PARSE_SCRIPT, &error, &tree), GLTANG_ERR_LIMIT);
  EXPECT_EQ(tree, nullptr);
}

TEST(TreeDepth, DeeplyNestedCallsAreRefusedToo) {
  std::string source = "f";
  for (int i = 0; i < 20000; ++i) {
    source += "(f";
  }
  for (int i = 0; i < 20000; ++i) {
    source += ")";
  }
  source += ";";
  GLTANG_ParseError error = {0, 0, {0}};
  GLTANG_Tree * tree = nullptr;
  EXPECT_EQ(gltang_parse(source.c_str(), GLTANG_PARSE_SCRIPT, &error, &tree), GLTANG_ERR_LIMIT);
  EXPECT_EQ(tree, nullptr);
}

TEST(TreeDepth, PrintingATallTreeIsLinearNotQuadratic) {
  // A quadratic print writes an indent proportional to the depth for every
  // node: 10,000 nodes at an average of 5,000 levels is 50 MB of spaces.
  GLTANG_ParseError error = {0, 0, {0}};
  GLTANG_Tree * tree = nullptr;
  std::string source = chain(GLTANG_MAX_TREE_DEPTH - 1);
  ASSERT_EQ(gltang_parse(source.c_str(), GLTANG_PARSE_SCRIPT, &error, &tree), GLTANG_OK);
  // The printer writes to stdout; point stdout at a file to measure it.
  // Restores fd 1 on every path out of the scope.
  struct Redirect {
    FILE * file = tmpfile();
    int saved = -1;
    Redirect() {
      if (file) {
        fflush(stdout);
        saved = dup(1);
        if (saved >= 0) {
          dup2(fileno(file), 1);
        }
      }
    }
    ~Redirect() {
      restore();
      if (file) {
        fclose(file);
      }
    }
    void restore() {
      if (saved >= 0) {
        fflush(stdout);
        dup2(saved, 1);
        close(saved);
        saved = -1;
      }
    }
  } redirect;
  ASSERT_NE(redirect.file, nullptr);
  ASSERT_GE(redirect.saved, 0);
  gltang_tree_print(tree);
  redirect.restore();
  long length = ftell(redirect.file);
  gltang_tree_destroy(tree);
  EXPECT_GT(length, 10000);
  EXPECT_LT(length, 10000L * 1200) << "the indent is capped";
}

// ---------------------------------------------------------------------------
// Allocation failure
// ---------------------------------------------------------------------------

TEST(AllocationFailure, EachAllocationCompileMakesFailedInTurnGivesAnAnswerAndLeaksNothing) {
  // A function body long enough that its code buffer must grow.
  std::string long_body = "x = 0;";
  for (int i = 0; i < 60; ++i) {
    long_body += " x = x + " + std::to_string(i) + ";";
  }
  const std::string long_source = long_body + " x;";
  // A ranged for emits a two-word instruction; one of these paddings puts its
  // second word exactly where the code buffer is full and must grow.
  std::vector<std::string> padded;
  for (int pad = 0; pad < 12; ++pad) {
    std::string source = "x = 0;";
    for (int i = 0; i < pad; ++i) {
      source += " x = 0;";
    }
    padded.push_back(source + " for (e : [1, 2]) { x = e; } x;");
  }
  std::vector<const char *> sources = {
    "1 + 2;",
    long_source.c_str(),
    "function f(a, b) { return a * b + 1; } x = [f(1, 2), \"s\" + \"t\", {k: 1.5}]; for (i = 0; i < 3; i += 1) { x[0] = x[0] + i; } x;",
    "use a.b as c; function g(n) { if (n < 1) { return 0; } return g(n - 1); } g(3);",
  };
  for (const std::string & p : padded) {
    sources.push_back(p.c_str());
  }
  for (const char * source : sources) {
    GLTANG_ParseError error = {0, 0, {0}};
    GLTANG_Tree * tree = nullptr;
    ASSERT_EQ(gltang_parse(source, GLTANG_PARSE_SCRIPT, &error, &tree), GLTANG_OK) << source;
    bool undisturbed = false;
    int oom = 0;
    for (long n = 1; n <= 20000; ++n) {
      GLTANG_Program * program = nullptr;
      alloc_sweep::track(true);
      alloc_sweep::arm(n);
      GLTANG_Result result = gltang_compile(tree, "t.tang", &error, &program);
      alloc_sweep::disarm();
      bool fired = alloc_sweep::fired();
      if (result == GLTANG_OK) {
        EXPECT_NE(program, nullptr);
        gltang_program_release(program);
      }
      else {
        EXPECT_EQ(program, nullptr) << source << " n=" << n;
      }
      EXPECT_EQ(alloc_sweep::live(), 0u) << source << ": failing allocation " << n << " left blocks behind";
      alloc_sweep::track(false);
      if (!fired) {
        EXPECT_EQ(result, GLTANG_OK) << source << " n=" << n;
        undisturbed = true;
        break;
      }
      EXPECT_EQ(result, GLTANG_ERR_OOM) << source << ": allocation " << n << " failed and compile said " << gltang_result_string(result);
      oom += result == GLTANG_ERR_OOM;
    }
    EXPECT_TRUE(undisturbed) << source;
    EXPECT_GT(oom, 3) << source;
    gltang_tree_destroy(tree);
  }
}

TEST(AllocationFailure, EachAllocationARunMakesFailedInTurnGivesAnAnswerAndLeaksNothing) {
  const char * sources[] = {
    "print(\"hi\"); 1 + 2;",
    "a = []; for (i = 0; i < 8; i += 1) { a[i] = [i, \"v\" + i, {k: i * 1.5}]; } print(a); a;",
    "function f(n) { if (n < 1) { return \"x\"; } return f(n - 1) + \"y\"; } m = {a: f(4), b: [1, 2, 3] * 3}; m;",
    "s = \"abc\"; t = s[1:] + s[::-1]; use missing; [t, missing, 7 / 0];",
    // Alternating encodings in a container: the text of it has a segment per element.
    "a = []; for (i = 0; i < 20; i += 1) { a[i] = i % 2 == 0 ? \"x\" : \"y\".html; } s = a as string; print(s); s.length;",
  };
  for (const char * source : sources) {
    tt::Compiled compiled(source);
    ASSERT_TRUE(compiled.ok()) << source;
    std::string baseline;
    {
      tt::Context context(compiled.program);
      ASSERT_TRUE(context.execute()) << source;
      baseline = context.describe() + "|" + context.raw();
    }
    bool undisturbed = false;
    int failures = 0;
    for (long n = 1; n <= 5000; ++n) {
      tt::Config config;
      config.fail_at = n;
      tt::Context context(compiled.program, config);
      bool fired_late = false;
      if (context.ok()) {
        context.execute();
        fired_late = true;
      }
      if (!context.tracker.fired) {
        if (fired_late) {
          EXPECT_TRUE(context.has_run);
          EXPECT_EQ(context.describe() + "|" + context.raw(), baseline) << source << " n=" << n;
        }
        undisturbed = true;
        break;
      }
      ++failures;
      if (context.ok() && context.ran == GRCORE_OK && context.outcome == GRCORE_OUTCOME_FINISHED) {
        // The run absorbed the failure: it must then be an error value, never
        // a quietly different answer.
        std::string got = context.describe() + "|" + context.raw();
        EXPECT_TRUE(got == baseline || context.is_error() || context.is_array() || context.is_map() || context.is_integer())
            << source << " n=" << n << ": " << got;
      }
    }
    EXPECT_TRUE(undisturbed) << source << ": the sweep never reached an undisturbed run";
    EXPECT_GT(failures, 3) << source;
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
