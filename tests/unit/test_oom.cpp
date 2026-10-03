/**
 * @file
 *
 * Allocation failure is a code path: fail each allocation the parser makes,
 * one at a time, and require an answer, no crash and no leak.
 *
 * test_helpers.cpp routes every plain malloc, calloc and realloc the library
 * makes (its own, flex's, bison's) through wrappers that can fail the Nth.
 * Failing every call from some point on would only show the first check
 * working, because the structure that is allocated first is also checked
 * first; failing exactly one call reaches each arm on its own. Allocations
 * cutil makes inside its own shared object are not reachable this way, and
 * ASan and Valgrind (make test-asan, make test-valgrind-quiet) are what watch
 * the rest.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"
#include "oracle/oracle.h"

namespace {

const std::string kCorpus = std::string(GLTANG_TEST_DATA) + "/corpus";

struct Subject {
  std::string name;
  std::string source;
  GLTANG_ParseMode mode;
};

std::vector<Subject> subjects() {
  std::vector<Subject> s;
  for (const char * file : {
      "script/tests-first-program.tang", "script/map-literal.tang",
      "script/string-escapes.tang", "script/function-nested.tang",
      "script/for-ranged.tang", "script/use-as.tang", "script/slice.tang",
      "script/cast.tang", "script/integer-min-magnitude.tang",
      "script/reject-invalid-utf8-string.tang",
      "script/reject-unterminated-string.tang",
      "script/reject-compound-assign-on-index.tang",
      "template/control-flow.tang", "template/string-types.tang",
      "template/function-in-template.tang",
      "template/reject-invalid-utf8-before-print-tag-expression.tang",
      "template/reject-syntax-error-in-tag.tang",
      }) {
    std::string f = file;
    s.push_back({f, read_file(kCorpus + "/" + f), f.compare(0, 7, "script/") == 0 ? GLTANG_PARSE_SCRIPT : GLTANG_PARSE_TEMPLATE});
  }
  s.push_back({"a long template", std::string(3000, 'x') + "<%= 1 %>" + std::string(3000, 'y'), GLTANG_PARSE_TEMPLATE});
  s.push_back({"a long string", "x = \"" + std::string(5000, 'z') + "\";", GLTANG_PARSE_SCRIPT});
  return s;
}

} // namespace

TEST(AllocationFailure, EachAllocationFailedInTurnGivesAnAnswerAndLeaksNothing) {
  for (const Subject & subject : subjects()) {
    // What the source does when nothing fails.
    GLTANG_Result expected;
    {
      Parsed baseline(subject.source, subject.mode);
      expected = baseline.result;
      ASSERT_TRUE(expected == GLTANG_OK || expected == GLTANG_ERR_FORMAT) << subject.name;
    }
    long total = 0;
    int oom_count = 0;
    bool reached_undisturbed_run = false;
    for (long n = 1; n <= 20000; n++) {
      GLTANG_ParseError error = {0, 0, {0}};
      GLTANG_Tree * tree = nullptr;
      alloc_sweep::track(true);
      alloc_sweep::arm(n);
      GLTANG_Result result = gltang_parse(subject.source.c_str(), subject.mode, &error, &tree);
      alloc_sweep::disarm();
      bool fired = alloc_sweep::fired();
      total = alloc_sweep::calls();
      if (result == GLTANG_OK) {
        EXPECT_NE(tree, nullptr) << subject.name << " n=" << n;
        gltang_tree_destroy(tree);
      }
      else {
        EXPECT_EQ(tree, nullptr) << subject.name << " n=" << n;
      }
      EXPECT_EQ(alloc_sweep::live(), 0u) << subject.name << ": failing allocation " << n << " left blocks behind";
      alloc_sweep::track(false);
      if (!fired) {
        // n is past the last allocation: the run was undisturbed.
        EXPECT_EQ(result, expected) << subject.name << " n=" << n;
        reached_undisturbed_run = true;
        break;
      }
      // An allocation that failed and was delivered must not be swallowed: a
      // parse that "succeeds" after losing memory has dropped part of the tree.
      EXPECT_NE(result, GLTANG_OK) << subject.name << ": allocation " << n << " failed and the parse still succeeded";
      EXPECT_TRUE(result == GLTANG_ERR_OOM || result == GLTANG_ERR_FORMAT) << subject.name << " n=" << n << " gave " << gltang_result_string(result);
      // A source that parses cleanly has no syntax error to report, so a lost
      // allocation there can only be out of memory.
      if (expected == GLTANG_OK) {
        EXPECT_EQ(result, GLTANG_ERR_OOM) << subject.name << " n=" << n;
      }
      oom_count += result == GLTANG_ERR_OOM;
    }
    EXPECT_GT(total, 3) << subject.name << " made suspiciously few allocations";
    EXPECT_GT(oom_count, 0) << subject.name << ": no failed allocation was reported as out of memory";
    EXPECT_TRUE(reached_undisturbed_run) << subject.name << ": the sweep never reached a run with no failed allocation";
  }
}

TEST(AllocationFailure, TheFirstAllocationFailingIsOutOfMemoryAndChangesNothing) {
  GLTANG_Tree * sentinel = reinterpret_cast<GLTANG_Tree *>(0x3);
  GLTANG_Tree * tree = sentinel;
  GLTANG_ParseError error = {7, 8, "keep"};
  alloc_sweep::track(true);
  alloc_sweep::arm(1);
  GLTANG_Result result = gltang_parse("1 + 2", GLTANG_PARSE_SCRIPT, &error, &tree);
  alloc_sweep::disarm();
  EXPECT_EQ(result, GLTANG_ERR_OOM);
  EXPECT_EQ(tree, sentinel);
  EXPECT_EQ(error.line, 7);
  EXPECT_EQ(alloc_sweep::live(), 0u);
  alloc_sweep::track(false);
}

TEST(AllocationFailure, TheScannersOwnBufferGrowthIsReportedAsOutOfMemoryNotASyntaxError) {
  // The scanner's failure to grow a token buffer reaches the parser as a token
  // it has no rule for, which bison reports as a syntax error. The result must
  // still be OOM. A long string forces the growth path.
  std::string source = "x = \"" + std::string(5000, 'a') + "\";";
  int seen_oom = 0;
  for (long n = 1; n < 400; n++) {
    alloc_sweep::arm(n);
    Parsed parsed(source, GLTANG_PARSE_SCRIPT);
    alloc_sweep::disarm();
    if (!alloc_sweep::fired()) {
      break;
    }
    EXPECT_EQ(parsed.result, GLTANG_ERR_OOM) << "n=" << n << ": " << parsed.error.message;
    seen_oom += parsed.result == GLTANG_ERR_OOM;
  }
  EXPECT_GT(seen_oom, 3);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
