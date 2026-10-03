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
#include "exec_harness.h"

#include <memory>

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

// ---------------------------------------------------------------------------
// The host API under allocation failure (story 10)
// ---------------------------------------------------------------------------
//
// Each allocation the context makes is failed in turn (the harness's tracker
// serves the group's allocator and page provider): library resolution, a native
// call, a template call, a scope exhaustion, an error-list entry. Every run
// must come to an answer - a correct result, an Out of memory value, or ERR_OOM
// - and leave no block behind (Context::destroy checks that), and the first run
// in which no allocation was failed must be the baseline's.

namespace {

struct TemplateDef {
  const char * name;
  const char * source;
  uint64_t fuel;
  GLTANG_ScopePolicy policy;
};

bool oom_native(GLTANG_NativeCall * call, void *) {
  gltang_call_return_string(call, "native<", 7, GLTANG_UNICODE_STRING_TYPE_HTML);
  return true;
}

struct Outcome {
  std::string raw;
  size_t errors = 0;
  bool finished = false;
};

/// Runs the page with the nth allocation failed (0: none). `fired` says whether
/// the failure was delivered.
Outcome run_with_failure(const std::string & page_source, const std::vector<TemplateDef> & templates, long n, bool log_all, bool * fired) {
  Outcome outcome;
  tt::Compiled page(page_source, tt::Mode::Script, "page.tang");
  std::vector<std::unique_ptr<tt::Compiled>> parts;
  for (const TemplateDef & def : templates) {
    parts.push_back(std::make_unique<tt::Compiled>(def.source, tt::Mode::Script, (std::string(def.name) + ".tang").c_str()));
  }
  tt::Config config;
  config.fail_at = n;
  tt::Context context(page.program, config);
  if (context.ok()) {
    GLTANG_Library * root = context.library();
    GLTANG_Library * user = nullptr;
    if (gltang_library_create("user", &user) == GLTANG_OK) {
      gltang_library_add_integer(user, "id", 7);
      gltang_library_add_string(user, "name", "u<", 2, GLTANG_UNICODE_STRING_TYPE_HTML);
      gltang_library_add_library(root, user);
      gltang_library_release(user);
    }
    gltang_library_add_native(root, "native", oom_native, nullptr);
    for (size_t i = 0; i < templates.size(); ++i) {
      gltang_library_add_template(root, templates[i].name, parts[i]->program, templates[i].fuel, templates[i].policy);
    }
    gltang_execution_set_name(context.execution, "page");
    gltang_execution_set_log_all_errors(context.execution, log_all);
    outcome.finished = context.execute();
    outcome.raw = context.raw();
    outcome.errors = context.error_count();
    // Whatever happened, the readers answer.
    (void)context.describe();
    (void)context.rendered();
  }
  *fired = context.tracker.fired;
  return outcome;
}

void sweep(const char * name, const std::string & page_source, const std::vector<TemplateDef> & templates, bool log_all = false) {
  bool fired = false;
  Outcome baseline = run_with_failure(page_source, templates, 0, log_all, &fired);
  ASSERT_FALSE(fired) << name;
  ASSERT_TRUE(baseline.finished) << name << ": the undisturbed run must finish";
  bool reached_undisturbed_run = false;
  int degraded = 0;
  for (long n = 1; n <= 5000; ++n) {
    Outcome o = run_with_failure(page_source, templates, n, log_all, &fired);
    if (!fired) {
      EXPECT_EQ(o.raw, baseline.raw) << name << " n=" << n;
      EXPECT_EQ(o.errors, baseline.errors) << name << " n=" << n;
      reached_undisturbed_run = true;
      break;
    }
    degraded += o.raw != baseline.raw || o.errors != baseline.errors || !o.finished;
  }
  EXPECT_TRUE(reached_undisturbed_run) << name << ": the sweep never reached a run with no failed allocation";
  EXPECT_GT(degraded, 0) << name << ": failing allocations never changed an answer, so the sweep reached nothing";
}

}  // namespace

TEST(AllocationFailure, LibraryResolutionAndANativeCall) {
  sweep("use and native",
      "use user; use math; use native; use user.id as id; use nope; x = native(); print(x); print(user.name); print(id); print(math.pi); print(nope); print(native() + \"|\");",
      {});
}

TEST(AllocationFailure, ARandomGeneratorAndASeededOne) {
  sweep("random", "use random; r = random.seeded(5); print(r.next_int); print(random.global.next_int != random.default.next_int); r.set_seed(3);", {});
}

TEST(AllocationFailure, ATemplateCall) {
  sweep("template call", "use t; print(\"[\" + t() + \"]\"); print(t()); print(t());",
      {{"t", "print(\"<b>\" + !\"<i>\"); print(\"x\");", 10000, GLTANG_SCOPE_EMPTY}});
}

TEST(AllocationFailure, ANestedCallAScopeExhaustionAndTheErrorListEntries) {
  sweep("scope exhaustion", "use sidebar; print(\"<main>\" + sidebar()); s = \"abc\"; s[0] = \"x\"; print(1 / 0); print(\"end\");",
      {{"sidebar", "use nav; print(nav()); print(\"<aside>\"); print(1 / 0);", 10000, GLTANG_SCOPE_EMPTY},
       {"nav", "print(\"part\"); while (true) {}", 300, GLTANG_SCOPE_SEGMENTS}});
}

TEST(AllocationFailure, AScopeStoppedInsideANativeOperation) {
  sweep("native in a scope", "use nav; print(\"a\" + nav() + \"b\");",
      {{"nav", "x = [0] * 200000; print(\"unreachable\");", 300, GLTANG_SCOPE_EMPTY}});
}

TEST(AllocationFailure, LogEveryErrorAtCreation) {
  sweep("log all", "x = 1 / 0; y = 2 % 0; print(x); use t; print(t());",
      {{"t", "print(1 / 0);", 1000, GLTANG_SCOPE_EMPTY}}, true);
}

TEST(AllocationFailure, TheLibraryAndTheSeedSequenceUnderTheProcessAllocator) {
  // These are plain cutil memory, built before any context exists: the sweep
  // is the one the parser's uses.
  bool reached = false;
  for (long n = 1; n <= 200; ++n) {
    alloc_sweep::track(true);
    alloc_sweep::arm(n);
    GLTANG_Library * root = nullptr;
    GLTANG_Library * child = nullptr;
    GLTANG_SeedSequence * seeds = nullptr;
    bool all = gltang_library_create(nullptr, &root) == GLTANG_OK;
    all = all && gltang_library_add_integer(root, "a", 1) == GLTANG_OK;
    all = all && gltang_library_add_string(root, "s", "text", 4, GLTANG_UNICODE_STRING_TYPE_TRUSTED) == GLTANG_OK;
    all = all && gltang_library_add_native(root, "n", oom_native, nullptr) == GLTANG_OK;
    all = all && gltang_library_create("child", &child) == GLTANG_OK;
    all = all && gltang_library_add_float(child, "f", 1.5) == GLTANG_OK;
    all = all && gltang_library_add_library(root, child) == GLTANG_OK;
    all = all && gltang_seeds_create(1, &seeds) == GLTANG_OK;
    alloc_sweep::disarm();
    bool fired = alloc_sweep::fired();
    EXPECT_EQ(all, !fired) << "n=" << n << ": a failed allocation must be reported, and a clean run must succeed";
    gltang_library_release(child);
    gltang_library_release(root);
    gltang_seeds_destroy(seeds);
    EXPECT_EQ(alloc_sweep::live(), 0u) << "failing allocation " << n << " left blocks behind";
    alloc_sweep::track(false);
    if (!fired) {
      reached = true;
      break;
    }
  }
  EXPECT_TRUE(reached);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
