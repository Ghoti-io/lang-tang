// Template calls and budget scopes (CAP-2; AD-21, AD-23): a template is a value
// `use` binds, calling it is a guest-to-guest call that opens a runtime-core
// budget scope, and a runaway template is stopped at its own boundary.

#include "exec_harness.h"

#include <memory>
#include <thread>

using tt::Compiled;
using tt::Config;
using tt::Context;

namespace {

const uint64_t kUnlimited = GRCORE_UNLIMITED;

struct Part {
  std::string name;
  std::string source;
  uint64_t fuel = 10000;
  GLTANG_ScopePolicy policy = GLTANG_SCOPE_EMPTY;
  tt::Mode mode = tt::Mode::Script;
};

/// A page, the templates it can call, and a context that runs the page.
class Site {
 public:
  std::unique_ptr<Compiled> page;
  std::vector<std::unique_ptr<Compiled>> compiled_parts;
  std::unique_ptr<Context> context;

  Site(const std::string & page_source, const std::vector<Part> & parts, const Config & config = Config(), tt::Mode page_mode = tt::Mode::Script) {
    page = std::make_unique<Compiled>(page_source, page_mode, "page.tang");
    EXPECT_TRUE(page->ok()) << page->error.message;
    context = std::make_unique<Context>(page->program, config);
    EXPECT_TRUE(context->ok());
    EXPECT_EQ(gltang_execution_set_name(context->execution, "page"), GLTANG_OK);
    for (const Part & part : parts) {
      compiled_parts.push_back(std::make_unique<Compiled>(part.source, part.mode, (part.name + ".tang").c_str()));
      EXPECT_TRUE(compiled_parts.back()->ok()) << part.name << ": " << compiled_parts.back()->error.message;
      EXPECT_EQ(gltang_library_add_template(context->library(), part.name.c_str(), compiled_parts.back()->program, part.fuel, part.policy), GLTANG_OK);
    }
  }
  Context & operator*() { return *context; }
  Context * operator->() { return context.get(); }
};

// The golden example of the design notes.
const char * kPage = "use sidebar;\nprint(\"<main>\" + sidebar());";
const char * kSidebar = "use nav; print(nav()); print(\"<aside>\");";
const char * kNav = "while (true) {}";

std::vector<Part> page_parts(uint64_t page_fuel, uint64_t sidebar_fuel, uint64_t nav_fuel, GLTANG_ScopePolicy nav_policy, const char * nav_source = kNav) {
  (void)page_fuel;
  return {
    {"sidebar", kSidebar, sidebar_fuel, GLTANG_SCOPE_EMPTY, tt::Mode::Script},
    {"nav", nav_source, nav_fuel, nav_policy, tt::Mode::Script},
  };
}

}  // namespace

// ---------------------------------------------------------------------------
// A template call
// ---------------------------------------------------------------------------

TEST(Template, ACallRunsTheTemplateAndItsValueIsWhatItPrintedWithEachSegmentTagged) {
  Site site("use sidebar; print(\"[\" + sidebar() + \"]\");",
      {{"sidebar", "print(\"<b>\" + !\"<i>\");", 1000, GLTANG_SCOPE_EMPTY, tt::Mode::Script}});
  ASSERT_TRUE(site->execute());
  EXPECT_EQ(site->rendered(), "[<b>&lt;i&gt;]");
  EXPECT_EQ(site->raw(), "[<b><i>]");
  EXPECT_EQ(site->error_count(), 0u);
}

TEST(Template, ATemplateWritesWithTemplateTextAndTagsToo) {
  Site site("use row; print(\"<ul>\"); for (i = 0; i < 3; i += 1) { print(row()); } print(\"</ul>\");",
      {{"row", "<li><%= \"a&b\" %></li>", 1000, GLTANG_SCOPE_EMPTY, tt::Mode::Template}});
  ASSERT_TRUE(site->execute());
  EXPECT_EQ(site->raw(), "<ul><li>a&b</li><li>a&b</li><li>a&b</li></ul>");
}

TEST(Template, AValueIsAStringAndKeepsItsSegmentsWhenStoredAndAppended) {
  Site site("use t; x = t(); y = x + \"|\" + x; print(y); [x.length, y.length];",
      {{"t", "print(\"a\" + !\"<\"); print(\"é\");", 1000, GLTANG_SCOPE_EMPTY, tt::Mode::Script}});
  ASSERT_TRUE(site->execute());
  EXPECT_EQ(site->rendered(), "a&lt;é|a&lt;é");
  EXPECT_EQ(site->describe(), "[3, 7]");
}

TEST(Template, ATemplateWithNoOutputIsTheEmptyString) {
  Site site("use t; x = t(); [x, x.length, x == \"\"];", {{"t", "y = 1 + 1;", 1000, GLTANG_SCOPE_EMPTY, tt::Mode::Script}});
  ASSERT_TRUE(site->execute());
  EXPECT_EQ(site->describe(), "[, 0, true]");
}

TEST(Template, EachCallHasItsOwnVariablesAndTheCallersAreUntouched) {
  Site site("use t; x = 5; total = 0; print(t()); print(t()); print(x); print(total);",
      {{"t", "if (x == null) { x = 0; } x = x + 1; print(x);", 1000, GLTANG_SCOPE_EMPTY, tt::Mode::Script}});
  ASSERT_TRUE(site->execute());
  EXPECT_EQ(site->raw(), "1150") << "x is 1 in each call, and the page's x and total are what they were";
}

TEST(Template, AFunctionIsStoredAndCalledLater) {
  Site site("use t; f = t; print(f()); print(f());", {{"t", "print(\"x\");", 1000, GLTANG_SCOPE_EMPTY, tt::Mode::Script}});
  ASSERT_TRUE(site->execute());
  EXPECT_EQ(site->raw(), "xx");
}

TEST(Template, AnArgumentIsAMismatchAndTheTemplateDoesNotRun) {
  Site site("use t; a = t(1); b = t(1, 2); print([a, b] as string); c = t; print(c());",
      {{"t", "print(\"ran\");", 1000, GLTANG_SCOPE_EMPTY, tt::Mode::Script}});
  ASSERT_TRUE(site->execute());
  EXPECT_EQ(site->raw(), "[Error: Argument Count Mismatch, Error: Argument Count Mismatch]ran");
}

TEST(Template, ATemplateIsAFunctionValueThatPrintsAsNothingAndCannotBeCompared) {
  Site site("use t; print(t); [t, t == t, t as string, !t];", {{"t", "1;", 1000, GLTANG_SCOPE_EMPTY, tt::Mode::Script}});
  ASSERT_TRUE(site->execute());
  EXPECT_EQ(site->raw(), "");
  EXPECT_EQ(site->describe(), "[Function(0), Error: Not supported, Error: Not supported, false]");
}

TEST(Template, ATemplateThatCallsItselfStopsAtTheDepthBudget) {
  Config config;
  config.calls = 6;
  Site site("use self; print(self());", {{"self", "use self; print(\"<\"); print(self()); print(\">\");", 100000, GLTANG_SCOPE_EMPTY, tt::Mode::Script}}, config);
  ASSERT_TRUE(site->execute());
  // The page is a frame, and six more nest: the seventh call is the error.
  EXPECT_EQ(site->raw(), "<<<<<<>>>>>>");
  ASSERT_EQ(site->error_count(), 1u);
  EXPECT_EQ(site->error(0).e.kind, GLTANG_ERROR_RECURSION_LIMIT);
  EXPECT_EQ(site->error(0).chain.size(), 6u);
}

TEST(Template, TheDepthIsCountedInFramesAcrossTemplatesAndFunctions) {
  Config config;
  config.calls = 4;
  Site site("use t; function f(n) { if (n == 0) { return t(); } return f(n - 1); } print(f(2)); print(\"|\");",
      {{"t", "function g(n) { if (n == 0) { return 1; } return g(n - 1); } print(g(3));", 1000, GLTANG_SCOPE_EMPTY, tt::Mode::Script}}, config);
  ASSERT_TRUE(site->execute());
  // page(1) f f f(4 with 3 nested calls) is at the limit: t cannot be entered.
  EXPECT_EQ(site->raw(), "|");
  EXPECT_EQ(site->error_count(), 1u);
}

TEST(Template, ATemplateResolvesThroughTheLibrariesTheExecutionCarries) {
  Site site("use t; use user; print(user.name + \":\" + t());",
      {{"t", "use user; print(user.name + \"!\");", 1000, GLTANG_SCOPE_EMPTY, tt::Mode::Script}});
  GLTANG_Library * user = nullptr;
  ASSERT_EQ(gltang_library_create("user", &user), GLTANG_OK);
  ASSERT_EQ(gltang_library_add_string(user, "name", "ann", 3, GLTANG_UNICODE_STRING_TYPE_TRUSTED), GLTANG_OK);
  ASSERT_EQ(gltang_library_add_library(site->library(), user), GLTANG_OK);
  gltang_library_release(user);
  ASSERT_TRUE(site->execute());
  EXPECT_EQ(site->raw(), "ann:ann!");
}

TEST(Template, ATemplateReachesALibraryItsOwnProgramCarries) {
  Site site("use t; print(t());", {{"t", "use extra; print(extra.n);", 1000, GLTANG_SCOPE_EMPTY, tt::Mode::Script}});
  GLTANG_Library * root = nullptr;
  GLTANG_Library * extra = nullptr;
  ASSERT_EQ(gltang_library_create(nullptr, &root), GLTANG_OK);
  ASSERT_EQ(gltang_library_create("extra", &extra), GLTANG_OK);
  ASSERT_EQ(gltang_library_add_integer(extra, "n", 42), GLTANG_OK);
  ASSERT_EQ(gltang_library_add_library(root, extra), GLTANG_OK);
  gltang_library_release(extra);
  // The template's own program is not shared yet: this one reference is the library's and ours.
  Compiled own("use extra; print(extra.n);", tt::Mode::Script, "own.tang");
  ASSERT_EQ(gltang_program_set_libraries(own.program, root), GLTANG_OK);
  gltang_library_release(root);
  GLTANG_Library * more = nullptr;
  ASSERT_EQ(gltang_library_create("holder", &more), GLTANG_OK);
  ASSERT_EQ(gltang_library_add_template(more, "own", own.program, 1000, GLTANG_SCOPE_EMPTY), GLTANG_OK);
  Compiled page("use holder; use holder.own as own; print(own());", tt::Mode::Script, "page.tang");
  Context context(page.program);
  ASSERT_EQ(gltang_library_add_library(context.library(), more), GLTANG_OK);
  gltang_library_release(more);
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(context.raw(), "42");
}

TEST(Template, ATemplateInASubLibraryIsReachedByAPath) {
  Compiled inner("print(\"deep\");", tt::Mode::Script, "inner.tang");
  GLTANG_Library * lib = nullptr;
  ASSERT_EQ(gltang_library_create("views", &lib), GLTANG_OK);
  ASSERT_EQ(gltang_library_add_template(lib, "inner", inner.program, 1000, GLTANG_SCOPE_EMPTY), GLTANG_OK);
  Compiled page("use views.inner as t; print(t());");
  Context context(page.program);
  ASSERT_EQ(gltang_library_add_library(context.library(), lib), GLTANG_OK);
  gltang_library_release(lib);
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(context.raw(), "deep");
}

TEST(Template, TheProgramIsSharedAndRunsInManyContexts) {
  Compiled shared("print(\"s\");", tt::Mode::Script, "shared.tang");
  GLTANG_Library * lib = nullptr;
  ASSERT_EQ(gltang_library_create(nullptr, &lib), GLTANG_OK);
  ASSERT_EQ(gltang_library_add_template(lib, "t", shared.program, 1000, GLTANG_SCOPE_EMPTY), GLTANG_OK);
  Compiled page("use t; print(t() + t());");
  std::atomic<int> good{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back([&]() {
      Context context(page.program);
      if (context.ok() && gltang_execution_set_libraries(context.execution, lib) == GLTANG_OK) {
        context.attached = true;
        if (context.execute() && context.raw() == "ss") {
          ++good;
        }
      }
    });
  }
  for (auto & t : threads) {
    t.join();
  }
  gltang_library_release(lib);
  EXPECT_EQ(good.load(), 4);
}

// ---------------------------------------------------------------------------
// Nested runaway (CAP-2): the nav pane is stopped at its own boundary
// ---------------------------------------------------------------------------

TEST(Scope, ARunawayNavPaneIsStoppedAndThePageAndSidebarFinish) {
  Site site(kPage, page_parts(10000, 10000, 500, GLTANG_SCOPE_EMPTY));
  ASSERT_TRUE(site->execute()) << "the host process survives, and so does the run";
  EXPECT_EQ(site->raw(), "<main><aside>");
  ASSERT_EQ(site->error_count(), 1u);
  auto e = site->error(0);
  EXPECT_EQ(e.e.kind, GLTANG_ERROR_LIMIT_EXCEEDED);
  EXPECT_EQ(e.e.how, GLTANG_ERROR_HOW_SCOPE_LIMIT);
  EXPECT_EQ(e.template_name(), "nav");
  EXPECT_EQ(e.chain_text(), "page:2 sidebar:1");
  EXPECT_EQ(e.chain[0].file, std::string("page.tang"));
  EXPECT_EQ(e.chain[1].file, std::string("sidebar.tang"));
  EXPECT_EQ(e.file(), "nav.tang");
  EXPECT_EQ(e.e.line, 1);
  // Everything is closed again.
  EXPECT_EQ(grcore_context_fuel_scope_depth(site->context), 0u);
  EXPECT_EQ(grcore_stack_frame_count(grcore_context_stack(site->context)), 0u);
  EXPECT_EQ(grcore_context_depth(site->context, GRCORE_DEPTH_GUEST), 0u);
}

TEST(Scope, TheCallsValueIsTheLimitErrorAndItIsNotListedAgainWhenPrinted) {
  Site site("use nav; x = nav(); print(x); print(x); x;", {{"nav", kNav, 400, GLTANG_SCOPE_EMPTY, tt::Mode::Script}});
  ASSERT_TRUE(site->execute());
  ASSERT_TRUE(site->is_error());
  EXPECT_EQ(site->error_kind(), GLTANG_ERROR_LIMIT_EXCEEDED);
  EXPECT_EQ(site->error_count(), 1u) << "the scope limit is the entry; the error value is not a second one";
  // The limit error's origin is the call site, in the caller's file.
  auto origin = site->error_origin();
  EXPECT_STREQ(origin.file, "page.tang");
  EXPECT_EQ(origin.line, 1);
}

TEST(Scope, TheStoppedPanesBudgetIsItsOwnAndTheOthersAreUntouched) {
  // The nav pane is paused at its own limit, and the sidebar and the page are
  // shown to hold their allowances.
  Site site(kPage, page_parts(10000, 10000, 500, GLTANG_SCOPE_PAUSE));
  ASSERT_FALSE(site->execute());
  ASSERT_TRUE(site->paused());
  GRCORE_Context * c = site->context;
  ASSERT_EQ(grcore_context_fuel_scope_depth(c), 2u);
  uint64_t nav = grcore_context_fuel_scope_top(c);
  uint64_t sidebar = nav - 1u;
  uint64_t sidebar_remaining = 0, nav_remaining = 1;
  ASSERT_EQ(grcore_context_fuel_scope_remaining(c, sidebar, &sidebar_remaining), GRCORE_OK);
  ASSERT_EQ(grcore_context_fuel_scope_remaining(c, nav, &nav_remaining), GRCORE_OK);
  EXPECT_EQ(nav_remaining, 0u);
  EXPECT_GT(sidebar_remaining, 9900u) << "the sidebar's clock stopped while the nav pane ran";
  // The ceiling is the sum of what was charged: the page, the sidebar and the nav pane.
  EXPECT_GT(grcore_context_fuel_used(c), 500u);
}

TEST(Scope, TheSegmentsPolicyLeavesTheCompletedSegmentsInTheSlot) {
  Site site(kPage, page_parts(10000, 10000, 500, GLTANG_SCOPE_SEGMENTS, "print(\"one\"); print(\"two\"); while (true) {}"));
  ASSERT_TRUE(site->execute());
  EXPECT_EQ(site->raw(), "<main>onetwo<aside>");
  ASSERT_EQ(site->error_count(), 1u) << "the entry is still recorded";
  EXPECT_EQ(site->error(0).e.how, GLTANG_ERROR_HOW_SCOPE_LIMIT);
  EXPECT_EQ(site->error(0).template_name(), "nav");
}

TEST(Scope, TheSegmentsPolicyCutsBetweenWholePrintsAndKeepsEncodings) {
  // The last prints are huge strings whose append is stopped part-way by the
  // scope; the slot holds the completed prints and none of the bytes of the
  // one that was cut.
  Site site("use nav; print(\"[\" + nav() + \"]\");",
      {{"nav", "print(\"a<\"); print(!\"b<\"); big = \"y\"; for (i = 0; i < 13; i += 1) { big = big + big; } for (i = 0; i < 1000000; i += 1) { print(big); }",
        3000, GLTANG_SCOPE_SEGMENTS, tt::Mode::Script}});
  ASSERT_TRUE(site->execute());
  std::string rendered = site->rendered();
  ASSERT_GT(rendered.size(), 8u + 8192u);
  EXPECT_EQ(rendered.compare(0, 8, "[a<b&lt;"), 0) << rendered.substr(0, 40);
  EXPECT_EQ(rendered.back(), ']');
  EXPECT_EQ((rendered.size() - 1 - 8) % 8192, 0u) << "whole prints of 8,192 bytes, never part of one";
  EXPECT_EQ(site->error_count(), 1u);
}

TEST(Scope, ThePauseDevelopmentPolicyPausesAtTheCalleesFileAndLine) {
  Site site(kPage, page_parts(10000, 10000, 500, GLTANG_SCOPE_PAUSE));
  ASSERT_FALSE(site->execute());
  ASSERT_TRUE(site->paused());
  GRCORE_Location where = grcore_context_pause_location(site->context);
  EXPECT_STREQ(where.file, "nav.tang");
  EXPECT_EQ(where.line, 1);
  ASSERT_GE(grcore_context_pause_key_count(site->context), 1u);
  EXPECT_STREQ(grcore_context_pause_key(site->context, 0)->name, grcore_core_key(GRCORE_REQUEST_FUEL)->name);
  EXPECT_EQ(site->error_count(), 0u) << "a pause is not a limit yet";
}

TEST(Scope, ThePausedScopeIsRaisedAndTheCallFinishes) {
  Site site(kPage, page_parts(10000, 10000, 200, GLTANG_SCOPE_PAUSE,
      "n = 0; while (n < 300) { n += 1; } print(\"nav\");"));
  ASSERT_FALSE(site->execute());
  ASSERT_TRUE(site->paused());
  GRCORE_Context * c = site->context;
  EXPECT_STREQ(grcore_context_pause_location(c).file, "nav.tang");
  // The host raises the innermost scope with what it has used so far and more.
  uint64_t top = grcore_context_fuel_scope_top(c);
  uint64_t used = 0;
  ASSERT_EQ(grcore_context_fuel_scope_used(c, top, &used), GRCORE_OK);
  ASSERT_EQ(grcore_context_fuel_scope_set_budget(c, top, used + 100000), GRCORE_OK);
  EXPECT_TRUE(site->resume());
  EXPECT_EQ(site->raw(), "<main>nav<aside>");
  EXPECT_EQ(site->error_count(), 0u);
}

TEST(Scope, ThePausedRunIsUnwoundWithTheLimitAndTheProcessSurvives) {
  Site site(kPage, page_parts(10000, 10000, 500, GLTANG_SCOPE_PAUSE));
  ASSERT_FALSE(site->execute());
  GRCORE_Context * c = site->context;
  ASSERT_EQ(grcore_context_terminate(c), GRCORE_OK);
  GRCORE_Result r = grcore_resume(c, &site->outcome);
  EXPECT_EQ(r, GRCORE_ERR_LIMIT);
  EXPECT_EQ(grcore_context_unwind_result(c), GRCORE_ERR_LIMIT);
  EXPECT_EQ(gltang_execution_state(site->execution), GLTANG_EXECUTION_UNWOUND);
  EXPECT_EQ(grcore_stack_frame_count(grcore_context_stack(c)), 0u);
  EXPECT_EQ(grcore_context_fuel_scope_depth(c), 0u);
  EXPECT_EQ(grcore_context_depth(c, GRCORE_DEPTH_GUEST), 0u);
  EXPECT_TRUE(site->is_null());
}

TEST(Scope, ATerminateInsideATemplateEndsTheWholeRunAndNoScopeCatchesIt) {
  Site site(kPage, page_parts(10000, 10000, kUnlimited, GLTANG_SCOPE_EMPTY));
  ASSERT_EQ(grcore_context_terminate(site->context), GRCORE_OK);
  EXPECT_FALSE(site->execute());
  EXPECT_EQ(site->ran, GRCORE_ERR_LIMIT);
  EXPECT_EQ(site->error_count(), 0u) << "a terminate is not a scope limit";
}

// ---------------------------------------------------------------------------
// The inclusive request budget
// ---------------------------------------------------------------------------

TEST(Scope, APageThatCalls10000ChildrenIsStoppedByTheRequestBudget) {
  Config config;
  config.fuel = 5000;
  Site site("use child; for (i = 0; i < 10000; i += 1) { child(); } print(\"done\");",
      {{"child", "1;", 100000, GLTANG_SCOPE_EMPTY, tt::Mode::Script}}, config);
  ASSERT_FALSE(site->execute());
  ASSERT_TRUE(site->paused()) << "the ceiling is a pause";
  ASSERT_GE(grcore_context_pause_key_count(site->context), 1u);
  EXPECT_STREQ(grcore_context_pause_key(site->context, 0)->name, grcore_core_key(GRCORE_REQUEST_FUEL)->name);
  EXPECT_EQ(site->error_count(), 0u) << "no child was stopped by its own scope";
  // The host gives up on it.
  ASSERT_EQ(grcore_context_terminate(site->context), GRCORE_OK);
  EXPECT_EQ(grcore_resume(site->context, &site->outcome), GRCORE_ERR_LIMIT);
  EXPECT_EQ(site->error_count(), 0u);
}

TEST(Scope, RaisingTheCeilingLetsThePageFinish) {
  Config config;
  config.fuel = 5000;
  Site site("use child; for (i = 0; i < 10000; i += 1) { child(); } print(\"done\");",
      {{"child", "1;", 100000, GLTANG_SCOPE_EMPTY, tt::Mode::Script}}, config);
  ASSERT_FALSE(site->execute());
  ASSERT_TRUE(site->paused());
  grcore_context_set_fuel(site->context, GRCORE_UNLIMITED);
  EXPECT_TRUE(site->resume());
  EXPECT_EQ(site->raw(), "done");
  EXPECT_EQ(site->error_count(), 0u);
}

TEST(Scope, ChildrenThatEachSpendTheirWholeBudgetAreStoppedByTheirOwnAndNotTheCeiling) {
  Config config;
  config.fuel = 1000000;
  Site site("use child; for (i = 0; i < 20; i += 1) { child(); } print(\"done\");",
      {{"child", "while (true) {}", 100, GLTANG_SCOPE_EMPTY, tt::Mode::Script}}, config);
  ASSERT_TRUE(site->execute());
  EXPECT_EQ(site->raw(), "done");
  EXPECT_EQ(site->error_count(), 20u);
  for (size_t i = 0; i < 20; ++i) {
    EXPECT_EQ(site->error(i).e.how, GLTANG_ERROR_HOW_SCOPE_LIMIT);
  }
}

// ---------------------------------------------------------------------------
// A native inside the scope
// ---------------------------------------------------------------------------

TEST(Scope, ANativeOperationInTheCalleeIsStoppedByTheScopeAndTheHalfBuiltValueIsReleased) {
  Site site("use nav; print(\"page \"); x = nav(); print(\"|\"); print(\"end\");",
      {{"nav", "x = [0] * 4000000; print(\"unreachable\");", 500, GLTANG_SCOPE_EMPTY, tt::Mode::Script}});
  ASSERT_TRUE(site->execute());
  EXPECT_EQ(site->raw(), "page |end");
  ASSERT_EQ(site->error_count(), 1u);
  EXPECT_EQ(site->error(0).template_name(), "nav");
  EXPECT_EQ(site->error(0).e.how, GLTANG_ERROR_HOW_SCOPE_LIMIT);
}

TEST(Scope, StringBuildingInTheCalleeIsStoppedByTheScopeAndTheCollectorKeepsThePage) {
  Site site("use nav; keep = [1, 2, 3]; s = nav(); print(keep.size); print(\"x\");",
      {{"nav", "s = \"x\"; while (true) { s = s + s; }", 800, GLTANG_SCOPE_EMPTY, tt::Mode::Script}});
  ASSERT_TRUE(site->execute());
  EXPECT_EQ(site->raw(), "3x");
  EXPECT_EQ(site->error_count(), 1u);
}

TEST(Scope, APrintOfAHugeContainerInTheCalleeIsStoppedByTheScope) {
  Site site("use nav; print(nav()); print(\"end\");",
      {{"nav", "a = [1, 2, 3, 4, 5, 6, 7, 8] * 1000; for (i = 0; i < 12; i += 1) { a = a + a; } print(a); print(a); print(a);", 3000, GLTANG_SCOPE_EMPTY, tt::Mode::Script}});
  ASSERT_TRUE(site->execute());
  EXPECT_EQ(site->raw(), "end");
  EXPECT_EQ(site->error_count(), 1u);
}

// ---------------------------------------------------------------------------
// Errors across the boundary
// ---------------------------------------------------------------------------

TEST(Template, ABrokenFinalExpressionOfACalledTemplateIsAnEntryNamingBoth) {
  Site site("use t;\nprint(\"[\" + t() + \"]\");", {{"t", "print(\"a\");\n1 / 0;", 1000, GLTANG_SCOPE_EMPTY, tt::Mode::Script}});
  ASSERT_TRUE(site->execute());
  EXPECT_EQ(site->raw(), "[a]");
  ASSERT_EQ(site->error_count(), 1u);
  auto e = site->error(0);
  EXPECT_EQ(e.e.how, GLTANG_ERROR_HOW_TEMPLATE_RESULT);
  EXPECT_EQ(e.e.kind, GLTANG_ERROR_DIVIDE_BY_ZERO);
  EXPECT_EQ(e.template_name(), "t");
  EXPECT_EQ(e.file(), "t.tang");
  EXPECT_EQ(e.e.line, 2);
  EXPECT_EQ(e.chain_text(), "page:2");
}

TEST(Template, AnErrorSwallowedInsideANestedTemplateNamesItsChain) {
  Site site("use a;\nprint(a());", {
      {"a", "use b;\n\nprint(b());", 1000, GLTANG_SCOPE_EMPTY, tt::Mode::Script},
      {"b", "x = 1;\nprint(1 / 0);", 1000, GLTANG_SCOPE_EMPTY, tt::Mode::Script}});
  ASSERT_TRUE(site->execute());
  ASSERT_EQ(site->error_count(), 1u);
  auto e = site->error(0);
  EXPECT_EQ(e.e.how, GLTANG_ERROR_HOW_PRINTED);
  EXPECT_EQ(e.template_name(), "b");
  EXPECT_EQ(e.file(), "b.tang");
  EXPECT_EQ(e.e.line, 2);
  EXPECT_EQ(e.chain_text(), "page:2 a:3");
}

TEST(Template, UnderHaltOnErrorATemplatesErrorEndsTheWholeRun) {
  Site site("use t; print(\"a\"); print(t()); print(\"b\");", {{"t", "print(\"x\"); y = 1 / 0; print(\"y\");", 1000, GLTANG_SCOPE_EMPTY, tt::Mode::Script}});
  ASSERT_EQ(gltang_execution_set_halt_on_error(site->execution, true), GLTANG_OK);
  EXPECT_FALSE(site->execute());
  EXPECT_EQ(site->ran, GRCORE_ERR_GUEST);
  EXPECT_EQ(site->raw(), "a");
  ASSERT_EQ(site->error_count(), 1u);
  EXPECT_EQ(site->error(0).template_name(), "t");
  EXPECT_EQ(site->error(0).chain_text(), "page:1");
  EXPECT_EQ(grcore_context_fuel_scope_depth(site->context), 0u);
}

TEST(Template, UnderLogAllTheEntryIsMadeAtCreationInTheTemplateThatMadeIt) {
  Site site("use t; print(t());", {{"t", "x = 1 / 0;\nprint(x);", 1000, GLTANG_SCOPE_EMPTY, tt::Mode::Script}});
  ASSERT_EQ(gltang_execution_set_log_all_errors(site->execution, true), GLTANG_OK);
  ASSERT_TRUE(site->execute());
  ASSERT_EQ(site->error_count(), 1u);
  EXPECT_EQ(site->error(0).e.how, GLTANG_ERROR_HOW_CREATED);
  EXPECT_EQ(site->error(0).template_name(), "t");
  EXPECT_EQ(site->error(0).chain_text(), "page:1");
}

// ---------------------------------------------------------------------------
// The collector and the moving stack, with templates in flight
// ---------------------------------------------------------------------------

TEST(Template, TheValuesOfACallerSurviveACalleeThatAllocatesAndCollects) {
  Site site("use t; keep = [\"a\", \"b\", [1, 2, 3]]; s = \"\"; for (i = 0; i < 5; i += 1) { s = s + t(); } print(s); keep;",
      {{"t", "xs = []; for (i = 0; i < 100; i += 1) { xs[i] = \"item\" + i; } print(xs.size); print(\"-\");", 100000, GLTANG_SCOPE_EMPTY, tt::Mode::Script}});
  ASSERT_TRUE(site->execute());
  EXPECT_EQ(site->raw(), "100-100-100-100-100-");
  EXPECT_EQ(site->describe(), "[a, b, [1, 2, 3]]");
}

TEST(Template, ANestedCallSeesTheSameStackAfterTheStackMovedUnderIt) {
  Site site("use a; print(a());",
      {{"a", "use b; x = [1, 2]; print(b()); print(x.size);", 100000, GLTANG_SCOPE_EMPTY, tt::Mode::Script},
       {"b", "function f(n) { if (n == 0) { return 0; } return 1 + f(n - 1); } print(f(40));", 100000, GLTANG_SCOPE_EMPTY, tt::Mode::Script}});
  grcore_stack_set_always_move(grcore_context_stack(site->context), true);
  ASSERT_TRUE(site->execute());
  EXPECT_EQ(site->raw(), "402");
}

// ---------------------------------------------------------------------------
// The frame protocol names (program, function, offset) for every frame
// ---------------------------------------------------------------------------

TEST(Frames, AWalkOfAPausedNestedCallNamesEachFramesProgramFileAndLine) {
  Site site(kPage, page_parts(10000, 10000, 500, GLTANG_SCOPE_PAUSE,
      "function spin(n) {\n  while (true) {}\n}\nspin(1);"));
  ASSERT_FALSE(site->execute());
  ASSERT_TRUE(site->paused());
  GRCORE_FrameWalk walk;
  ASSERT_EQ(grcore_frame_walk_begin(site->context, &walk), GRCORE_OK);
  std::vector<std::string> frames;
  std::vector<uint64_t> programs;
  GRCORE_AbstractFrame frame;
  while (grcore_frame_walk_next(&walk, &frame)) {
    frames.push_back(std::string(frame.location.file ? frame.location.file : "?") + ":" + std::to_string(frame.location.line));
    programs.push_back(frame.identity.function >> 32);
  }
  // spin (a function of nav), nav's top level, sidebar's, the page's.
  ASSERT_EQ(frames.size(), 4u);
  EXPECT_EQ(frames[0], "nav.tang:2");
  EXPECT_EQ(frames[1], "nav.tang:4");
  EXPECT_EQ(frames[2], "sidebar.tang:1");
  EXPECT_EQ(frames[3], "page.tang:2");
  EXPECT_EQ(programs[0], programs[1]);
  EXPECT_NE(programs[1], programs[2]);
  EXPECT_NE(programs[2], programs[3]);
  EXPECT_EQ(programs[3], 0u) << "the main program is program 0";
}

TEST(Frames, TheVariablesOfEachFrameAreThoseOfItsOwnActivation) {
  Site site("use a; x = \"page\"; print(a());",
      {{"a", "use b; x = \"a\"; print(b());", 100000, GLTANG_SCOPE_EMPTY, tt::Mode::Script},
       {"b", "x = \"b\"; while (true) {}", 500, GLTANG_SCOPE_PAUSE, tt::Mode::Script}});
  ASSERT_FALSE(site->execute());
  ASSERT_TRUE(site->paused());
  GRCORE_FrameWalk walk;
  ASSERT_EQ(grcore_frame_walk_begin(site->context, &walk), GRCORE_OK);
  std::vector<std::string> seen;
  GRCORE_AbstractFrame frame;
  while (grcore_frame_walk_next(&walk, &frame)) {
    size_t scopes = grcore_frame_scope_count(&frame);
    ASSERT_GE(scopes, 1u);
    GRCORE_ScopeInfo info;
    ASSERT_EQ(grcore_frame_scope(&frame, scopes - 1u, &info), GRCORE_OK);
    ASSERT_EQ(info.kind, GRCORE_SCOPE_GLOBAL);
    for (size_t v = 0; v < info.variable_count; ++v) {
      GRCORE_Variable variable;
      ASSERT_EQ(grcore_frame_variable(&frame, scopes - 1u, v, &variable), GRCORE_OK);
      if (variable.name && std::string(variable.name) == "x") {
        char text[64] = {0};
        frame.descriptor->inspect(frame.context, variable.kind, variable.value, text, sizeof(text));
        seen.push_back(text);
      }
    }
  }
  EXPECT_EQ(seen, (std::vector<std::string>{"b", "a", "page"})) << "each frame reads the x of its own activation";
}

// ---------------------------------------------------------------------------
// CAP-3: pause inside a scope and a nested template, resume on another thread
// ---------------------------------------------------------------------------

namespace {

std::string migrate_run(bool migrate, std::string * errors) {
  Config config;
  config.fuel = 400;
  Site site("use a; x = 0; for (i = 0; i < 3; i += 1) { x += i; print(a()); } print(x);",
      {{"a", "use b; print(\"<a\"); print(b()); print(\"a>\");", 100000, GLTANG_SCOPE_EMPTY, tt::Mode::Script},
       {"b", "n = 0; s = \"\"; while (n < 200) { n += 1; s = s + \"z\"; } print(s.length); y = 1 / 0; print(y);", 100000, GLTANG_SCOPE_EMPTY, tt::Mode::Script}},
      config);
  bool done = site->execute();
  int hops = 0;
  while (!done && migrate) {
    EXPECT_EQ(grcore_context_fuel_scope_depth(site->context) <= 2u, true);
    EXPECT_EQ(grcore_context_release(site->context), GRCORE_OK);
    GRCORE_Result acquired = GRCORE_ERR_INTERNAL;
    std::thread t([&]() {
      acquired = grcore_context_acquire(site->context);
      if (acquired == GRCORE_OK) {
        grcore_context_set_fuel(site->context, grcore_context_fuel_used(site->context) + 450);
        done = site->resume();
        grcore_context_release(site->context);
      }
    });
    t.join();
    EXPECT_EQ(acquired, GRCORE_OK);
    EXPECT_EQ(grcore_context_acquire(site->context), GRCORE_OK);
    EXPECT_LT(++hops, 200);
  }
  if (!migrate) {
    while (!done) {
      grcore_context_set_fuel(site->context, grcore_context_fuel_used(site->context) + 450);
      done = site->resume();
    }
  }
  std::string out = site->raw();
  for (size_t i = 0; i < site->error_count(); ++i) {
    auto e = site->error(i);
    *errors += e.template_name() + ":" + std::to_string(e.e.line) + "[" + e.chain_text() + "]" + std::to_string(e.e.how) + ";";
  }
  out += "|" + site->describe();
  return out;
}

}  // namespace

TEST(Threads, AContextPausedInsideANestedTemplateResumesOnAnotherThreadWithTheSameResult) {
  std::string reference_errors, migrated_errors;
  std::string reference = migrate_run(false, &reference_errors);
  std::string migrated = migrate_run(true, &migrated_errors);
  EXPECT_EQ(migrated, reference);
  EXPECT_EQ(migrated_errors, reference_errors);
  EXPECT_FALSE(reference_errors.empty());
}

TEST(Threads, AContextPausedInsideAScopeResumesOnAnotherThreadWithTheScopesStillOpen) {
  Site site(kPage, page_parts(10000, 10000, 500, GLTANG_SCOPE_PAUSE, "n = 0; while (n < 400) { n += 1; } print(\"nav\");"));
  ASSERT_FALSE(site->execute());
  ASSERT_TRUE(site->paused());
  GRCORE_Context * c = site->context;
  ASSERT_EQ(grcore_context_release(c), GRCORE_OK);
  GRCORE_Result acquired = GRCORE_ERR_INTERNAL;
  uint64_t scopes_on_b = 0;
  bool finished = false;
  std::thread t([&]() {
    acquired = grcore_context_acquire(c);
    if (acquired == GRCORE_OK) {
      scopes_on_b = grcore_context_fuel_scope_depth(c);
      uint64_t top = grcore_context_fuel_scope_top(c);
      uint64_t used = 0;
      grcore_context_fuel_scope_used(c, top, &used);
      grcore_context_fuel_scope_set_budget(c, top, used + 100000);
      finished = site->resume();
      grcore_context_release(c);
    }
  });
  t.join();
  ASSERT_EQ(acquired, GRCORE_OK);
  ASSERT_EQ(grcore_context_acquire(c), GRCORE_OK);
  EXPECT_EQ(scopes_on_b, 2u);
  EXPECT_TRUE(finished);
  EXPECT_EQ(site->raw(), "<main>nav<aside>");
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
