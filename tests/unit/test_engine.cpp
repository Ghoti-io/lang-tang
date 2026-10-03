// The engine as a client of runtime-core and runtime-heap: polls, pause and
// resume, unwind, budgets, the frame protocol, the collector, and the places
// the engine's values leave it (result, output, error origins).
//
// Each test names what is true. The suite runs unchanged under the heap's
// torture and verify modes and with a guest stack that moves on every push
// (see exec_harness.h).

#include "exec_harness.h"
#include "test_helpers.h"
#include <ghoti.io/lang-tang/bytecode.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>

using tt::Compiled;
using tt::Config;
using tt::Context;
using tt::Mode;

namespace {

/// How many guest frames a test that recurses may use under the variant that
/// copies the stack on every push.
uint64_t deep(uint64_t normal, uint64_t moving) {
  return tt::moving_stack_requested() ? moving : normal;
}

const char * const RUNAWAY = "x = 1;\nwhile (true) {\n}\n";

}  // namespace

// ---------------------------------------------------------------------------
// CAP-1: a runaway loop comes back paused at its file and line
// ---------------------------------------------------------------------------

TEST(Pause, ARunawayLoopPausesAtTheLoopsFileAndLine) {
  Compiled compiled(RUNAWAY, Mode::Script, "runaway.tang");
  ASSERT_TRUE(compiled.ok());
  Config config;
  config.fuel = 1000;
  Context context(compiled.program, config);
  ASSERT_TRUE(context.ok());
  ASSERT_FALSE(context.execute());
  ASSERT_TRUE(context.paused());
  EXPECT_EQ(gltang_execution_state(context.execution), GLTANG_EXECUTION_PAUSED);
  ASSERT_GE(grcore_context_pause_key_count(context.context), 1u);
  EXPECT_EQ(grcore_context_pause_key(context.context, 0), grcore_core_key(GRCORE_REQUEST_FUEL));
  GRCORE_Location where = grcore_context_pause_location(context.context);
  ASSERT_NE(where.file, nullptr);
  EXPECT_STREQ(where.file, "runaway.tang");
  EXPECT_EQ(where.line, 2) << "the loop is on line 2";
}

TEST(Pause, ThePauseIsAtAPollIdentityTheProgramCanName) {
  Compiled compiled(RUNAWAY, Mode::Script, "runaway.tang");
  ASSERT_TRUE(compiled.ok());
  Config config;
  config.fuel = 500;
  Context context(compiled.program, config);
  ASSERT_FALSE(context.execute());
  GRCORE_PollIdentity identity;
  ASSERT_EQ(grcore_context_poll_identity(context.context, &identity), GRCORE_OK);
  EXPECT_EQ(identity.function, 0u);
  int line = 0;
  ASSERT_EQ(gltang_program_locate(compiled.program, identity.function, identity.offset, &line), GLTANG_OK);
  EXPECT_EQ(line, 2);
  // And the same place however many times it is reached: the identity is the
  // site, not the visit.
  ASSERT_EQ(grcore_context_set_fuel(context.context, 1500), GRCORE_OK);
  ASSERT_FALSE(context.resume());
  GRCORE_PollIdentity again;
  ASSERT_EQ(grcore_context_poll_identity(context.context, &again), GRCORE_OK);
  EXPECT_EQ(again.function, identity.function);
  EXPECT_EQ(again.offset, identity.offset);
}

TEST(Pause, RaisingTheBudgetAndResumingFinishesABoundedLoop) {
  const char * source = "s = 0; for (i = 0; i < 2000; i += 1) { s += i; print(i % 10); }\ns;";
  tt::Run reference(source);
  ASSERT_TRUE(reference.context.is_integer());

  Compiled compiled(source);
  ASSERT_TRUE(compiled.ok());
  Config config;
  config.fuel = 3000;
  Context context(compiled.program, config);
  ASSERT_TRUE(context.ok());
  ASSERT_FALSE(context.execute());
  ASSERT_TRUE(context.paused());
  int pauses = 1;
  while (!context.finished_after_raising(3000)) {
    ++pauses;
    ASSERT_LT(pauses, 100);
  }
  EXPECT_GT(pauses, 1);
  EXPECT_EQ(gltang_execution_state(context.execution), GLTANG_EXECUTION_FINISHED);
  EXPECT_EQ(context.integer(), reference.context.integer());
  EXPECT_EQ(context.raw(), reference.context.raw());
}

TEST(Pause, UnwindingAPausedRunReturnsLimitAndTheProcessSurvives) {
  Compiled compiled(RUNAWAY);
  Config config;
  config.fuel = 800;
  Context context(compiled.program, config);
  ASSERT_FALSE(context.execute());
  ASSERT_TRUE(context.paused());
  ASSERT_EQ(grcore_context_terminate(context.context), GRCORE_OK);
  EXPECT_EQ(grcore_resume(context.context, &context.outcome), GRCORE_ERR_LIMIT);
  EXPECT_EQ(grcore_context_unwind_result(context.context), GRCORE_ERR_LIMIT);
  EXPECT_EQ(gltang_execution_state(context.execution), GLTANG_EXECUTION_UNWOUND);
  EXPECT_EQ(grcore_stack_frame_count(grcore_context_stack(context.context)), 0u) << "the unwinder emptied the stack";
  EXPECT_EQ(context.kind(), GLTANG_KIND_NULL);
  EXPECT_GE(gltang_execution_unwound_frames(context.execution), 1u);
}

TEST(Pause, AnUnwoundRunHasNoResultNotTheValueOfAnEarlierStatement) {
  Compiled compiled("1; while (true) { }");
  ASSERT_TRUE(compiled.ok());
  Config config;
  config.fuel = 300;
  Context context(compiled.program, config);
  ASSERT_FALSE(context.execute());
  ASSERT_TRUE(context.paused());
  ASSERT_EQ(grcore_context_terminate(context.context), GRCORE_OK);
  EXPECT_EQ(grcore_resume(context.context, &context.outcome), GRCORE_ERR_LIMIT);
  EXPECT_EQ(gltang_execution_state(context.execution), GLTANG_EXECUTION_UNWOUND);
  EXPECT_EQ(context.kind(), GLTANG_KIND_NULL) << "the 1 was not the program's answer";
}

TEST(Pause, UnwindingFromDeepInACallPopsEveryFrame) {
  const uint64_t depth = deep(400, 200);
  std::string source = "function d(n) { if (n <= 0) { while (true) { } } return d(n - 1); } d(" + std::to_string(depth) + ");";
  Compiled compiled(source);
  ASSERT_TRUE(compiled.ok());
  Config config;
  config.fuel = 100000;
  Context context(compiled.program, config);
  ASSERT_FALSE(context.execute());
  ASSERT_TRUE(context.paused());
  ASSERT_EQ(grcore_context_terminate(context.context), GRCORE_OK);
  EXPECT_EQ(grcore_resume(context.context, &context.outcome), GRCORE_ERR_LIMIT);
  // The program's own frame and the depth + 1 calls of d.
  EXPECT_EQ(gltang_execution_unwound_frames(context.execution), depth + 2u);
}

TEST(Pause, TheLimitReachedByAnUnwindVoteIsReadableFromTheContext) {
  Compiled compiled("function f(n) { return f(n + 1); } x = 0; while (true) { x += 1; }");
  Config config;
  config.fuel = 50;
  Context context(compiled.program, config);
  ASSERT_FALSE(context.execute());
  ASSERT_GE(grcore_context_pause_key_count(context.context), 1u);
  EXPECT_STREQ(grcore_context_pause_key(context.context, 0)->name, grcore_core_key(GRCORE_REQUEST_FUEL)->name);
}

// ---------------------------------------------------------------------------
// CAP-3: a paused context resumes on a different thread
// ---------------------------------------------------------------------------

namespace {

/// Acquires the context on this thread, gives it fuel and resumes it.
struct Hop {
  Context * context;
  uint64_t fuel;
  GRCORE_Result acquired = GRCORE_ERR_INTERNAL;
  bool finished = false;

  void operator()() {
    acquired = grcore_context_acquire(context->context) == GRCORE_OK ? GRCORE_OK : GRCORE_ERR_INVALID;
    if (acquired != GRCORE_OK) {
      return;
    }
    grcore_context_set_fuel(context->context, grcore_context_fuel_used(context->context) + fuel);
    finished = context->resume();
    grcore_context_release(context->context);
  }
};

}  // namespace

TEST(Threads, AContextPausedOnOneThreadResumesOnAnotherWithTheSameOutput) {
  const char * source =
    "function tri(n) { s = 0; for (i = 1; i <= n; i += 1) { s += i; } return s; }\n"
    "out = [];\n"
    "for (k = 1; k <= 40; k += 1) { out[k - 1] = tri(k); print(tri(k) % 7); }\n"
    "out;";
  tt::Run reference(source);
  ASSERT_TRUE(reference.context.is_array());
  std::string expected_output = reference.context.raw();
  std::string expected_result = reference.context.describe();

  Compiled compiled(source);
  Config config;
  config.fuel = 700;
  Context context(compiled.program, config);
  ASSERT_TRUE(context.ok());
  ASSERT_FALSE(context.execute());
  ASSERT_TRUE(context.paused());
  // Every hop pauses again until the program is done; each runs on a new thread.
  int hops = 0;
  bool finished = false;
  while (!finished) {
    ASSERT_EQ(grcore_context_release(context.context), GRCORE_OK);
    Hop hop{&context, 900};
    std::thread t([&hop]() { hop(); });
    t.join();
    ASSERT_EQ(hop.acquired, GRCORE_OK);
    ASSERT_EQ(grcore_context_acquire(context.context), GRCORE_OK);
    finished = hop.finished;
    ASSERT_LT(++hops, 500);
  }
  EXPECT_GT(hops, 1);
  EXPECT_EQ(context.raw(), expected_output);
  EXPECT_EQ(context.describe(), expected_result);
}

TEST(Threads, AnUnwoundRunOnAnotherThreadReportsTheLimit) {
  Compiled compiled(RUNAWAY);
  Config config;
  config.fuel = 400;
  Context context(compiled.program, config);
  ASSERT_FALSE(context.execute());
  ASSERT_EQ(grcore_context_release(context.context), GRCORE_OK);
  GRCORE_Result acquired = GRCORE_ERR_INTERNAL;
  GRCORE_Result terminated = GRCORE_ERR_INTERNAL;
  GRCORE_Result result = GRCORE_OK;
  std::thread t([&]() {
    acquired = grcore_context_acquire(context.context);
    if (acquired == GRCORE_OK) {
      terminated = grcore_context_terminate(context.context);
      result = grcore_resume(context.context, &context.outcome);
      grcore_context_release(context.context);
    }
  });
  t.join();
  ASSERT_EQ(grcore_context_acquire(context.context), GRCORE_OK);
  EXPECT_EQ(acquired, GRCORE_OK);
  EXPECT_EQ(terminated, GRCORE_OK);
  EXPECT_EQ(result, GRCORE_ERR_LIMIT);
  EXPECT_EQ(grcore_context_unwind_result(context.context), GRCORE_ERR_LIMIT);
  EXPECT_EQ(gltang_execution_state(context.execution), GLTANG_EXECUTION_UNWOUND);
  EXPECT_EQ(grcore_stack_frame_count(grcore_context_stack(context.context)), 0u);
  EXPECT_EQ(context.kind(), GLTANG_KIND_NULL);
}

// ---------------------------------------------------------------------------
// CAP-2: natives under a tiny budget reach a verdict
// ---------------------------------------------------------------------------

TEST(Natives, ARepetitionOfAHugeArrayIsStoppedByTheFuelNotByTheClock) {
  Compiled compiled("x = [0, 0, 0, 0] * 20000000; x.size;");
  ASSERT_TRUE(compiled.ok());
  Config config;
  config.fuel = 5000;
  Context context(compiled.program, config);
  ASSERT_TRUE(context.ok());
  GRCORE_Result r = grcore_run(context.context, gltang_execution_entry, context.execution, &context.outcome);
  // A native cannot pause (AD-21): the verdict is an unwind with the limit.
  EXPECT_EQ(r, GRCORE_ERR_LIMIT);
  EXPECT_EQ(grcore_context_unwind_result(context.context), GRCORE_ERR_LIMIT);
  ASSERT_GE(grcore_context_pause_key_count(context.context), 1u);
  EXPECT_EQ(grcore_context_pause_key(context.context, 0), grcore_core_key(GRCORE_REQUEST_FUEL));
  EXPECT_LT(grcore_context_fuel_used(context.context), 100000u) << "a bounded amount of work";
}

TEST(Natives, ARepetitionOfAHugeArrayUnderAMemoryBudgetRunsACollectionAndThenStops) {
  Compiled compiled("x = [0] * 1000000000; x;");
  ASSERT_TRUE(compiled.ok());
  Config config;
  config.memory_bytes = 8u << 20;
  Context context(compiled.program, config);
  ASSERT_TRUE(context.ok());
  GRCORE_Result r = grcore_run(context.context, gltang_execution_entry, context.execution, &context.outcome);
  // The allocation is refused after the collection, and the program goes on
  // with the error value in x.
  EXPECT_EQ(r, GRCORE_OK);
  EXPECT_EQ(context.outcome, GRCORE_OUTCOME_FINISHED);
  EXPECT_TRUE(context.is_error());
  EXPECT_EQ(context.error_kind(), GLTANG_ERROR_OUT_OF_MEMORY);
  GRHEAP_Stats stats;
  ASSERT_EQ(grheap_stats(context.heap, &stats), GRHEAP_OK);
  EXPECT_GT(stats.collections, 0u) << "a collection runs before the memory verdict is given";
}

TEST(Natives, BuildingAStringByDoublingIsStoppedInsideTheCopy) {
  Compiled compiled("s = \"xxxxxxxx\"; for (i = 0; i < 40; i += 1) { s = s + s; } s.length;");
  ASSERT_TRUE(compiled.ok());
  Config config;
  config.fuel = 200000;
  Context context(compiled.program, config);
  ASSERT_TRUE(context.ok());
  GRCORE_Result r = grcore_run(context.context, gltang_execution_entry, context.execution, &context.outcome);
  EXPECT_EQ(r, GRCORE_ERR_LIMIT);
  EXPECT_LT(grcore_context_memory_peak(context.context), 600u << 20) << "the copy that ran the fuel out was not the last one";
}

TEST(Natives, ADoublingStringInAnInfiniteLoopUnderAMemoryBudgetReachesAVerdict) {
  Compiled compiled("s = \"xxxxxxxx\"; while (true) { s = s + s; }");
  ASSERT_TRUE(compiled.ok());
  Config config;
  config.memory_bytes = 4u << 20;
  config.fuel = 2000000;  // a memory budget alone does not end a loop that survives the refusal
  Context context(compiled.program, config);
  ASSERT_TRUE(context.ok());
  GRCORE_Result r = grcore_run(context.context, gltang_execution_entry, context.execution, &context.outcome);
  // The loop polls on every pass, so the memory verdict reaches it there as a
  // pause; a copy that crosses the budget is unwound.
  EXPECT_TRUE((r == GRCORE_OK && context.outcome == GRCORE_OUTCOME_PAUSED) || r == GRCORE_ERR_LIMIT) << "r=" << r << " outcome=" << context.outcome;
  GRHEAP_Stats stats;
  ASSERT_EQ(grheap_stats(context.heap, &stats), GRHEAP_OK);
  EXPECT_GT(stats.collections, 0u) << "memory over budget runs a collection before the verdict";
}

namespace {

/// The fuel a whole run of `source` uses with no budget at all, and whether it finished.
uint64_t unlimited_cost(const std::string & source, bool * finished) {
  Compiled compiled(source);
  Context context(compiled.program);
  *finished = compiled.ok() && context.ok() && context.execute();
  return grcore_context_fuel_used(context.context);
}

/// Whether `source` is stopped by the limit when given `fuel`.
bool unwinds_with(const std::string & source, uint64_t fuel) {
  Compiled compiled(source);
  Config config;
  config.fuel = fuel;
  Context context(compiled.program, config);
  if (!compiled.ok() || !context.ok()) {
    return false;
  }
  GRCORE_Result r = grcore_run(context.context, gltang_execution_entry, context.execution, &context.outcome);
  return r == GRCORE_ERR_LIMIT && grcore_context_unwind_result(context.context) == GRCORE_ERR_LIMIT;
}

/// A native is paced if the fuel it alone uses is about its work, and a budget
/// that covers building its operand but not that work stops it. The operand is
/// built identically in both programs, so the difference is the native.
void expect_native_paced(const std::string & build, const std::string & native, uint64_t least_native_fuel) {
  bool built = false;
  bool whole = false;
  uint64_t build_cost = unlimited_cost(build + " 0;", &built);
  uint64_t total_cost = unlimited_cost(build + " " + native, &whole);
  ASSERT_TRUE(built) << build;
  ASSERT_TRUE(whole) << native;
  ASSERT_GE(total_cost, build_cost);
  EXPECT_GE(total_cost - build_cost, least_native_fuel) << native << " cost " << (total_cost - build_cost) << " on its own";
  EXPECT_FALSE(unwinds_with(build + " 0;", build_cost + 100)) << "the build alone fits the budget used below";
  EXPECT_TRUE(unwinds_with(build + " " + native, build_cost + least_native_fuel / 4)) << native << " was not stopped by a budget below its own work";
}

}  // namespace

TEST(Natives, PrintingAHugeContainerIsPacedByTheRuntimePoll) {
  expect_native_paced("a = [1, 2, 3] * 300000;", "print(a);", 20000);
}

TEST(Natives, ComparingTwoHugeArraysIsPaced) {
  expect_native_paced("a = [1, 2, 3] * 300000; b = [1, 2, 3] * 300000;", "a == b;", 10000);
}

TEST(Natives, SlicingAndReversingAHugeStringIsPaced) {
  expect_native_paced("s = \"abcdefgh\"; for (i = 0; i < 20; i += 1) { s = s + s; }", "t = s[::-1]; t.length;", 1800000);   // two passes of about 1M each
}

TEST(Natives, RenderingAContainerToTextIsPaced) {
  expect_native_paced("a = [1, 2, 3] * 300000;", "t = \"x\" + a; t.length;", 20000);
}

TEST(Natives, RenderingAContainerHoldingAHugeStringToTextIsPaced) {
  expect_native_paced("s = \"abcdefgh\"; for (i = 0; i < 20; i += 1) { s = s + s; } a = [s];", "t = \"x\" + a; t.length;", 200000);
}

TEST(Natives, ConcatenatingTwoHugeStringsIsPaced) {
  expect_native_paced("s = \"abcdefgh\"; for (i = 0; i < 20; i += 1) { s = s + s; } u = s[1:];", "t = s + u; t.length;", 200000);
}

TEST(Natives, DeepCopyingAHugeArrayIsPaced) {
  expect_native_paced("a = [1, 2, 3] * 300000;", "b = [a]; b.size;", 10000);
}

// ---------------------------------------------------------------------------
// Depth
// ---------------------------------------------------------------------------

TEST(Depth, RecursionPastTheBudgetIsTheErrorValueAndTheProgramContinues) {
  tt::Run run("function f(n) { return f(n + 1); } x = f(0); print(\"after\"); x;");
  EXPECT_TRUE(run.context.is_error());
  EXPECT_EQ(run.context.error_kind(), GLTANG_ERROR_RECURSION_LIMIT);
  EXPECT_EQ(run.context.raw(), "after");
}

TEST(Depth, ExactlyTheBudgetOfNestedCallsIsAllowedAndOneMoreIsNot) {
  Config config;
  config.calls = 512;
  tt::Run at("function d(n) { if (n <= 0) { return 0; } return d(n - 1); } d(511);", Mode::Script, config);
  EXPECT_TRUE(at.context.is_integer());
  tt::Run past("function d(n) { if (n <= 0) { return 0; } return d(n - 1); } d(512);", Mode::Script, config);
  EXPECT_TRUE(past.context.is_error());
}

TEST(Depth, FiftyThousandDeepRunsOnTheOrdinaryCStack) {
  const uint64_t n = deep(50000, 2000);
  Config config;
  config.calls = n + 10;
  std::string source = "function d(n) { if (n <= 0) { return 0; } return 1 + d(n - 1); } d(" + std::to_string(n) + ");";
  tt::Run run(source, Mode::Script, config);
  ASSERT_TRUE(run.context.is_integer());
  EXPECT_EQ(run.context.integer(), (int64_t)n);
}

TEST(Depth, ARunawayRecursionWithNoBudgetIsStoppedByFuelWithTheCStackUntouched) {
  Compiled compiled("function f(n) { return f(n + 1); } f(0);");
  Config config;
  config.calls = GRCORE_UNLIMITED;
  config.fuel = deep(2000000, 100000);
  Context context(compiled.program, config);
  ASSERT_TRUE(context.ok());
  ASSERT_FALSE(context.execute());
  EXPECT_TRUE(context.paused());
  EXPECT_GT(grcore_stack_frame_count(grcore_context_stack(context.context)), 1000u) << "frames are data on the guest stack";
}

// ---------------------------------------------------------------------------
// The frame protocol: the abstract frame of a paused engine
// ---------------------------------------------------------------------------

TEST(Frames, AMultiFrameStackReadsAsAbstractFramesWithLocationsAndScopes) {
  const char * source =
    "g = 7;\n"
    "function inner(a) {\n"
    "  local = a + 1;\n"
    "  while (true) { }\n"
    "}\n"
    "function outer(x) {\n"
    "  return inner(x * 2);\n"
    "}\n"
    "outer(5);\n";
  Compiled compiled(source, Mode::Script, "frames.tang");
  ASSERT_TRUE(compiled.ok());
  Config config;
  config.fuel = 600;
  Context context(compiled.program, config);
  ASSERT_FALSE(context.execute());
  ASSERT_TRUE(context.paused());
  ASSERT_TRUE(grcore_context_guest_state_readable(context.context));

  GRCORE_FrameWalk walk;
  ASSERT_EQ(grcore_frame_walk_begin(context.context, &walk), GRCORE_OK);
  GRCORE_AbstractFrame frame;
  ASSERT_TRUE(grcore_frame_walk_next(&walk, &frame));
  EXPECT_STREQ(frame.descriptor->name, "lang-tang");
  EXPECT_EQ(frame.depth, 0u);
  ASSERT_NE(frame.location.file, nullptr);
  EXPECT_STREQ(frame.location.file, "frames.tang");
  EXPECT_EQ(frame.location.line, 4) << "the innermost frame is at the loop";
  EXPECT_GT(frame.slot_count, 4u);

  // Header words are raw, everything after is a value.
  GRCORE_SlotKind kind;
  uint64_t value;
  ASSERT_EQ(grcore_frame_slot(&frame, 0, &kind, &value), GRCORE_OK);
  EXPECT_EQ(kind, GRCORE_SLOT_RAW);
  EXPECT_EQ(value, 1u) << "slot 0 is the function index; inner is function 1";
  ASSERT_EQ(grcore_frame_slot(&frame, 4, &kind, &value), GRCORE_OK);
  EXPECT_EQ(kind, GRCORE_SLOT_VALUE);

  // The scopes: inner's locals by name, then the program's.
  ASSERT_EQ(grcore_frame_scope_count(&frame), 2u);
  GRCORE_ScopeInfo scope;
  ASSERT_EQ(grcore_frame_scope(&frame, 0, &scope), GRCORE_OK);
  EXPECT_EQ(scope.kind, GRCORE_SCOPE_LOCAL);
  EXPECT_STREQ(scope.name, "inner");
  ASSERT_EQ(scope.variable_count, 2u);
  GRCORE_Variable variable;
  ASSERT_EQ(grcore_frame_variable(&frame, 0, 0, &variable), GRCORE_OK);
  EXPECT_STREQ(variable.name, "a");
  char text[64];
  size_t length = 0;
  ASSERT_EQ(grcore_engine_inspect(context.context, frame.engine, variable.kind, variable.value, text, sizeof(text), &length), GRCORE_OK);
  EXPECT_STREQ(text, "10");
  ASSERT_EQ(grcore_frame_variable(&frame, 0, 1, &variable), GRCORE_OK);
  EXPECT_STREQ(variable.name, "local");
  ASSERT_EQ(grcore_engine_inspect(context.context, frame.engine, variable.kind, variable.value, text, sizeof(text), &length), GRCORE_OK);
  EXPECT_STREQ(text, "11");
  ASSERT_EQ(grcore_frame_scope(&frame, 1, &scope), GRCORE_OK);
  EXPECT_EQ(scope.kind, GRCORE_SCOPE_GLOBAL);
  EXPECT_STREQ(scope.name, "program");
  ASSERT_EQ(scope.variable_count, 3u) << "g, inner, outer";
  bool found_g = false;
  for (size_t i = 0; i < scope.variable_count; ++i) {
    ASSERT_EQ(grcore_frame_variable(&frame, 1, i, &variable), GRCORE_OK);
    if (std::string(variable.name) == "g") {
      found_g = true;
      ASSERT_EQ(grcore_frame_inspect(&frame, 0, text, sizeof(text), &length), GRCORE_OK);
      ASSERT_EQ(grcore_engine_inspect(context.context, frame.engine, variable.kind, variable.value, text, sizeof(text), &length), GRCORE_OK);
      EXPECT_STREQ(text, "7");
    }
  }
  EXPECT_TRUE(found_g);

  // Outward: outer, called at line 7, and the program, at the call on line 9.
  ASSERT_TRUE(grcore_frame_walk_next(&walk, &frame));
  EXPECT_EQ(frame.depth, 1u);
  EXPECT_EQ(frame.location.line, 7);
  ASSERT_EQ(grcore_frame_scope(&frame, 0, &scope), GRCORE_OK);
  EXPECT_STREQ(scope.name, "outer");
  ASSERT_TRUE(grcore_frame_walk_next(&walk, &frame));
  EXPECT_EQ(frame.depth, 2u);
  EXPECT_EQ(frame.location.line, 9);
  EXPECT_EQ(grcore_frame_scope_count(&frame), 1u) << "the top level has only the program scope";
  EXPECT_FALSE(grcore_frame_walk_next(&walk, &frame));
}

TEST(Frames, TheInspectorShowsValuesAsTheLanguagePrintsThem) {
  Compiled compiled("a = [1, 2.5, \"s\", null, {k: true}]; b = \"text\"; c = 3.0; d = 12; function f(p) {} g = f; while (true) { }");
  Config config;
  config.fuel = 300;
  Context context(compiled.program, config);
  ASSERT_FALSE(context.execute());
  GRCORE_FrameWalk walk;
  ASSERT_EQ(grcore_frame_walk_begin(context.context, &walk), GRCORE_OK);
  GRCORE_AbstractFrame frame;
  ASSERT_TRUE(grcore_frame_walk_next(&walk, &frame));
  ASSERT_EQ(grcore_frame_scope_count(&frame), 1u);
  std::map<std::string, std::string> seen;
  GRCORE_ScopeInfo scope;
  ASSERT_EQ(grcore_frame_scope(&frame, 0, &scope), GRCORE_OK);
  for (size_t i = 0; i < scope.variable_count; ++i) {
    GRCORE_Variable v;
    ASSERT_EQ(grcore_frame_variable(&frame, 0, i, &v), GRCORE_OK);
    char text[128];
    size_t length;
    ASSERT_EQ(grcore_engine_inspect(context.context, frame.engine, v.kind, v.value, text, sizeof(text), &length), GRCORE_OK);
    seen[v.name] = text;
  }
  EXPECT_EQ(seen["a"], "[1, 2.5, s, null, {\"k\": true}]");
  EXPECT_EQ(seen["b"], "text");
  EXPECT_EQ(seen["c"], "3.");
  EXPECT_EQ(seen["d"], "12");
  EXPECT_EQ(seen["g"], "Function(1)");
}

TEST(Frames, TheEngineIsRegisteredOncePerContextWithALocateThatNamesTheSource) {
  Compiled compiled("x = 1;\ny = 2;\n", Mode::Script, "locate.tang");
  Context context(compiled.program);
  ASSERT_TRUE(context.ok());
  ASSERT_EQ(grcore_engine_count(context.context), 1u);
  const GRCORE_EngineDescriptor * descriptor = grcore_engine_descriptor(context.context, 1);
  ASSERT_NE(descriptor, nullptr);
  EXPECT_STREQ(descriptor->name, "lang-tang");
  ASSERT_NE(descriptor->locate, nullptr);
  GRCORE_Location where = descriptor->locate(context.context, 0, 0);
  EXPECT_STREQ(where.file, "locate.tang");
  EXPECT_EQ(where.line, 1);
  int last = 0;
  for (uint64_t offset = 0; offset < 16; ++offset) {
    last = std::max(last, descriptor->locate(context.context, 0, offset).line);
  }
  EXPECT_EQ(last, 2) << "a later offset is a later line";
  where = descriptor->locate(context.context, 99, 0);
  EXPECT_STREQ(where.file, "locate.tang");
  EXPECT_EQ(where.line, 0) << "a place that is not in the program has no line";
  EXPECT_NE(descriptor->unwind, nullptr);
  EXPECT_NE(descriptor->scopes.scope_count, nullptr);
}

TEST(Frames, AFramePushedByTheEngineNamesTheEngineAndTheStackIsEmptyAfterTheRun) {
  tt::Run run("function f() { return 1; } f();");
  EXPECT_EQ(grcore_stack_frame_count(grcore_context_stack(run.context.context)), 0u);
  EXPECT_EQ(gltang_execution_state(run.context.execution), GLTANG_EXECUTION_FINISHED);
}

// ---------------------------------------------------------------------------
// The collector
// ---------------------------------------------------------------------------

TEST(Collector, GarbageIsCollectedWhileTheProgramRuns) {
  Config config;
  config.gc_threshold = 16 * 1024;
  tt::Run run("keep = [1, 2, 3]; for (i = 0; i < 20000; i += 1) { t = [i, i + 1, {a: i}]; } keep;", Mode::Script, config);
  ASSERT_TRUE(run.context.is_array());
  EXPECT_EQ(run.context.describe(), "[1, 2, 3]");
  GRHEAP_Stats stats;
  ASSERT_EQ(grheap_stats(run.context.heap, &stats), GRHEAP_OK);
  EXPECT_GT(stats.collections, 0u);
  EXPECT_LT(stats.live_objects, 5000u) << "20,000 iterations of garbage did not stay live";
}

TEST(Collector, ValuesHeldInFramesGlobalsAndConstantsSurviveCollections) {
  Config config;
  config.gc_threshold = 0;  // collect before every allocation until something is live
  tt::Run run(
    "function make(n) { local = [n, \"str\" + n, {k: n}]; filler = [0] * 20; return local; }\n"
    "g = make(1);\n"
    "h = [make(2), make(3)];\n"
    "s = \"constant\" + \" string\";\n"
    "for (i = 0; i < 50; i += 1) { w = make(i); }\n"
    "[g, h, s, w];\n", Mode::Script, config);
  ASSERT_TRUE(run.context.is_array());
  EXPECT_EQ(run.context.describe(), "[[1, str1, {\"k\": 1}], [[2, str2, {\"k\": 2}], [3, str3, {\"k\": 3}]], constant string, [49, str49, {\"k\": 49}]]");
}

TEST(Collector, ABigProgramLeavesNothingBehindWhenTheContextIsDestroyed) {
  // Context::destroy proves the group handed out nothing it did not get back.
  tt::Run run("a = []; for (i = 0; i < 300; i += 1) { a[i] = [i, \"x\" * 1]; } a.size;");
  EXPECT_TRUE(run.context.is_integer());
}

TEST(Collector, ArenaModeKeepsEverythingUntilTheContextIsDestroyed) {
  Config config;
  config.arena = true;
  tt::Run run("a = []; for (i = 0; i < 2000; i += 1) { a[i] = [i, \"v\" + i]; } a.size;", Mode::Script, config);
  ASSERT_TRUE(run.context.is_integer());
  EXPECT_EQ(run.context.integer(), 2000);
  GRHEAP_Stats stats;
  ASSERT_EQ(grheap_stats(run.context.heap, &stats), GRHEAP_OK);
  EXPECT_EQ(stats.collections, 0u);
  EXPECT_GT(stats.live_objects, 2000u);
}

TEST(Collector, AHeapThatWasNotGivenTheCodecStillRunsCorrectly) {
  // A host that forgets gltang_heap_options_configure gets the identity codec:
  // a pointer is its own word, so everything works, and the integers are
  // counted as pointers that are not.
  Compiled compiled("a = [1, 2, 3]; b = 7; true; a;");
  tt::Tracker tracker;
  GRCORE_Group * group;
  ASSERT_EQ(grcore_group_create(&tracker.allocator, &tracker.pages, &group), GRCORE_OK);
  GRCORE_Context * context;
  ASSERT_EQ(grcore_context_create(group, nullptr, &context), GRCORE_OK);
  GRHEAP_Heap * heap;
  ASSERT_EQ(grheap_heap_create(context, nullptr, &heap), GRHEAP_OK);
  GLTANG_Execution * execution;
  ASSERT_EQ(gltang_execution_create(context, compiled.program, &execution), GLTANG_OK);
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(context, gltang_execution_entry, execution, &outcome), GRCORE_OK);
  ASSERT_EQ(grheap_collect(heap), GRHEAP_OK);
  EXPECT_EQ(gltang_execution_result_kind(execution), GLTANG_KIND_ARRAY);
  EXPECT_EQ(gltang_execution_result_size(execution), 3u);
  ASSERT_EQ(grcore_context_destroy(context), GRCORE_OK);
  ASSERT_EQ(grcore_group_destroy(group), GRCORE_OK);
  EXPECT_EQ(tracker.live_blocks, 0);
  EXPECT_EQ(tracker.live_pages, 0);
}

TEST(Collector, TheCodecTreatsImmediatesAsNotPointers) {
  GRHEAP_ValueCodec codec;
  gltang_value_codec(&codec);
  EXPECT_EQ(codec.tag_mask, 0xFu);
  EXPECT_EQ(codec.tag_value, 0u);
  EXPECT_EQ(codec.shift, 0u);
  EXPECT_EQ(codec.base, 0u);
  GRHEAP_Options * options;
  ASSERT_EQ(grheap_options_create(nullptr, &options), GRHEAP_OK);
  EXPECT_EQ(gltang_heap_options_configure(options), GLTANG_OK);
  GRHEAP_ValueCodec read;
  ASSERT_EQ(grheap_options_get_value_codec(options, &read), GRHEAP_OK);
  EXPECT_EQ(read.tag_mask, codec.tag_mask);
  grheap_options_destroy(options);
  EXPECT_EQ(gltang_heap_options_configure(nullptr), GLTANG_ERR_INVALID);
  gltang_value_codec(nullptr);  // ignored
}

// ---------------------------------------------------------------------------
// Execution lifecycle
// ---------------------------------------------------------------------------

TEST(Execution, ACreateWithoutAHeapOrTwiceOnOneContextIsRefused) {
  Compiled compiled("1;");
  tt::Tracker tracker;
  GRCORE_Group * group;
  ASSERT_EQ(grcore_group_create(&tracker.allocator, &tracker.pages, &group), GRCORE_OK);
  GRCORE_Context * context;
  ASSERT_EQ(grcore_context_create(group, nullptr, &context), GRCORE_OK);
  GLTANG_Execution * execution = nullptr;
  EXPECT_EQ(gltang_execution_create(context, compiled.program, &execution), GLTANG_ERR_INVALID) << "no heap";
  EXPECT_EQ(execution, nullptr);
  GRHEAP_Heap * heap;
  ASSERT_EQ(grheap_heap_create(context, nullptr, &heap), GRHEAP_OK);
  EXPECT_EQ(gltang_execution_create(nullptr, compiled.program, &execution), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_execution_create(context, nullptr, &execution), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_execution_create(context, compiled.program, nullptr), GLTANG_ERR_INVALID);
  ASSERT_EQ(gltang_execution_create(context, compiled.program, &execution), GLTANG_OK);
  GLTANG_Execution * second = nullptr;
  EXPECT_EQ(gltang_execution_create(context, compiled.program, &second), GLTANG_ERR_INVALID) << "one execution per context";
  EXPECT_EQ(second, nullptr);
  ASSERT_EQ(grcore_context_destroy(context), GRCORE_OK);
  ASSERT_EQ(grcore_group_destroy(group), GRCORE_OK);
  EXPECT_EQ(tracker.live_blocks, 0);
}

TEST(Execution, DestroyingItEarlyReleasesItsPartsAndLeavesTheAccessorsSafe) {
  Compiled compiled("print(\"x\"); [1, 2];");
  Context context(compiled.program);
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(context.raw(), "x");
  gltang_execution_destroy(context.execution);
  gltang_execution_destroy(context.execution);  // idempotent
  gltang_execution_destroy(nullptr);
  EXPECT_EQ(gltang_execution_result_kind(context.execution), GLTANG_KIND_NULL);
  EXPECT_EQ(context.raw(), "");
  EXPECT_EQ(context.rendered(), "");
  EXPECT_EQ(context.size(), 0u);
  EXPECT_EQ(gltang_execution_entry(context.context, context.execution), GRCORE_STEP_FINISHED) << "nothing to run";
}

TEST(Execution, TheDescriptorCallbacksAreSafeAfterTheExecutionIsDestroyed) {
  Compiled compiled("a = [1, \"two\"]; while (true) { }");
  ASSERT_TRUE(compiled.ok());
  Config config;
  config.fuel = 300;
  Context context(compiled.program, config);
  ASSERT_FALSE(context.execute());
  GRCORE_FrameWalk walk;
  ASSERT_EQ(grcore_frame_walk_begin(context.context, &walk), GRCORE_OK);
  GRCORE_AbstractFrame frame;
  ASSERT_TRUE(grcore_frame_walk_next(&walk, &frame));
  const GRCORE_EngineDescriptor * descriptor = grcore_engine_descriptor(context.context, 1);
  ASSERT_NE(descriptor, nullptr);
  // Take a heap value's word while the execution is alive.
  GRCORE_SlotKind kind;
  uint64_t word = 0;
  ASSERT_EQ(grcore_frame_slot(&frame, 4, &kind, &word), GRCORE_OK);

  gltang_execution_destroy(context.execution);

  char text[64];
  EXPECT_NO_FATAL_FAILURE((void)descriptor->inspect(context.context, GRCORE_SLOT_VALUE, word, text, sizeof(text)));
  EXPECT_EQ(descriptor->scopes.scope_count(&frame), 0u);
  GRCORE_ScopeInfo scope;
  EXPECT_NE(descriptor->scopes.scope(&frame, 0, &scope), GRCORE_OK);
  GRCORE_Variable variable;
  EXPECT_NE(descriptor->scopes.variable(&frame, 0, 0, &variable), GRCORE_OK);
  EXPECT_EQ(descriptor->locate(context.context, 0, 0).file, nullptr);
}

TEST(Execution, TheAccessorsAnswerForNullAndForAnExecutionThatNeverRan) {
  EXPECT_EQ(gltang_execution_result_kind(nullptr), GLTANG_KIND_NULL);
  EXPECT_FALSE(gltang_execution_result_bool(nullptr));
  EXPECT_EQ(gltang_execution_result_integer(nullptr), 0);
  EXPECT_EQ(gltang_execution_result_float(nullptr), 0.0);
  EXPECT_EQ(gltang_execution_result_text(nullptr, nullptr), nullptr);
  EXPECT_EQ(gltang_execution_result_size(nullptr), 0u);
  EXPECT_EQ(gltang_execution_state(nullptr), GLTANG_EXECUTION_NEW);
  EXPECT_EQ(gltang_execution_unwound_frames(nullptr), 0u);
  EXPECT_FALSE(gltang_execution_result_error(nullptr, nullptr, nullptr));
  char * text = nullptr;
  EXPECT_EQ(gltang_execution_result_describe(nullptr, &text, nullptr), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_execution_output_render(nullptr, &text, nullptr), GLTANG_ERR_INVALID);
  size_t length = 5;
  EXPECT_STREQ(gltang_execution_output_raw(nullptr, &length), "");
  EXPECT_EQ(length, 0u);
  gltang_buffer_free(nullptr);
  EXPECT_EQ(gltang_execution_set_resolver(nullptr, nullptr, nullptr), GLTANG_ERR_INVALID);

  Compiled compiled("1;");
  Context context(compiled.program);
  EXPECT_EQ(gltang_execution_state(context.execution), GLTANG_EXECUTION_NEW);
  EXPECT_EQ(context.kind(), GLTANG_KIND_NULL);
  EXPECT_EQ(context.raw(), "");
}

TEST(Execution, ARunThatEndedStaysEndedWhenRunAgain) {
  Compiled compiled("print(1);");
  Context context(compiled.program);
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(context.raw(), "1");
  EXPECT_TRUE(context.execute());
  EXPECT_EQ(context.raw(), "1") << "the program did not run twice";
}

TEST(Execution, AProgramIsSharedByContextsOnDifferentThreads) {
  Compiled compiled("function fib(n) { if (n < 2) { return n; } return fib(n - 1) + fib(n - 2); } print(fib(15));");
  ASSERT_TRUE(compiled.ok());
  std::atomic<int> good{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back([&]() {
      Context context(compiled.program);
      if (context.ok() && context.execute() && context.raw() == "610") {
        ++good;
      }
    });
  }
  for (auto & t : threads) {
    t.join();
  }
  EXPECT_EQ(good.load(), 4);
}

TEST(Execution, ProgramsAreReferenceCounted) {
  Compiled compiled("1;");
  ASSERT_TRUE(compiled.ok());
  GLTANG_Program * program = compiled.program;
  EXPECT_EQ(gltang_program_retain(program), program);
  gltang_program_release(program);
  gltang_program_release(nullptr);
  EXPECT_EQ(gltang_program_retain(nullptr), nullptr);
  Context context(program);
  gltang_program_release(program);  // drop the reference taken above; the execution holds its own
  gltang_program_retain(program);
  EXPECT_TRUE(context.execute());
}

// ---------------------------------------------------------------------------
// Where errors come from
// ---------------------------------------------------------------------------

TEST(ErrorOrigin, AnErrorValueRecordsTheFileAndLineItWasCreatedAt) {
  Compiled compiled("use x;\ny = 1 / 0;\n", Mode::Script, "origin.tang");
  Context context(compiled.program);
  ASSERT_TRUE(context.execute());
  ASSERT_TRUE(context.is_error());
  GLTANG_ErrorKind kind;
  GLTANG_ErrorOrigin origin;
  ASSERT_TRUE(gltang_execution_result_error(context.execution, &kind, &origin));
  EXPECT_EQ(kind, GLTANG_ERROR_DIVIDE_BY_ZERO);
  EXPECT_STREQ(origin.file, "origin.tang");
  EXPECT_EQ(origin.line, 2);
  EXPECT_EQ(origin.function, 0u);
  // The poll identity is a place the program can name.
  int line = 0;
  ASSERT_EQ(gltang_program_locate(compiled.program, origin.function, origin.offset, &line), GLTANG_OK);
  EXPECT_EQ(line, 2);
}

TEST(ErrorOrigin, AnErrorMadeInsideAFunctionNamesThatFunction) {
  Compiled compiled("function bad(a) {\n  return a % 0;\n}\nbad(1);\n", Mode::Script, "fn.tang");
  Context context(compiled.program);
  ASSERT_TRUE(context.execute());
  GLTANG_ErrorOrigin origin = context.error_origin();
  EXPECT_EQ(context.error_kind(), GLTANG_ERROR_MODULO_BY_ZERO);
  EXPECT_EQ(origin.function, 1u);
  EXPECT_EQ(origin.line, 2);
  EXPECT_STREQ(gltang_program_function_name(compiled.program, origin.function), "bad");
}

TEST(ErrorOrigin, AResultThatIsNotAnErrorHasNoOriginAndWritesNothing) {
  tt::Run run("1 + 1;");
  GLTANG_ErrorKind kind = GLTANG_ERROR_KIND_COUNT;
  GLTANG_ErrorOrigin origin = {nullptr, -1, 99, 99};
  EXPECT_FALSE(gltang_execution_result_error(run.context.execution, &kind, &origin));
  EXPECT_EQ(kind, GLTANG_ERROR_KIND_COUNT);
  EXPECT_EQ(origin.line, -1);
}

TEST(ErrorOrigin, EveryErrorKindHasATextAndTheMarkersArePrintedAsThemselves) {
  for (int k = 0; k < GLTANG_ERROR_KIND_COUNT; ++k) {
    const char * message = gltang_error_kind_message((GLTANG_ErrorKind)k);
    ASSERT_NE(message, nullptr);
    EXPECT_STRNE(message, "Unknown error") << k;
    EXPECT_EQ(gltang_error_kind_is_marker((GLTANG_ErrorKind)k), message[0] == '[') << message;
  }
  EXPECT_STREQ(gltang_error_kind_message(GLTANG_ERROR_KIND_COUNT), "Unknown error");
  EXPECT_FALSE(gltang_error_kind_is_marker(GLTANG_ERROR_KIND_COUNT));
}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

TEST(Output, TemplateTextIsTrustedAndOnlyPrintedValuesAreEncoded) {
  Compiled compiled("<p>Hello, <%= name %>! <b>bold</b></p>", Mode::Template);
  ASSERT_TRUE(compiled.ok());
  Context context(compiled.program);
  context.add_library("name", tt::Host::string("<script>", GLTANG_UNICODE_STRING_TYPE_HTML));
  // `use name;` is needed to bind the library into the template.
  Compiled with_use("<% use name; %><p>Hello, <%= name %>! <b>bold</b></p>", Mode::Template);
  ASSERT_TRUE(with_use.ok());
  Context host(with_use.program);
  host.add_library("name", tt::Host::string("<script>", GLTANG_UNICODE_STRING_TYPE_HTML));
  ASSERT_TRUE(host.execute());
  EXPECT_EQ(host.rendered(), "<p>Hello, &lt;script&gt;! <b>bold</b></p>");
  EXPECT_EQ(host.raw(), "<p>Hello, <script>! <b>bold</b></p>");
}

TEST(Output, EachEncodingIsAppliedToItsOwnSegment) {
  tt::Run run(
    "print(\"<a>\"); print(!\"<b>\"); print(%\"a b&c\"); print(\"<d>\".html_attribute);"
    "print(\"'q'\".javascript); print(\"x\".html);");
  EXPECT_EQ(run.context.rendered(), "<a>&lt;b&gt;a+b%26c&lt;d&gt;\\'q\\'x");
  EXPECT_EQ(run.context.raw(), "<a><b>a b&c<d>'q'x");
}

TEST(Output, AdjacentPiecesOfTheSameEncodingShareASegmentAndTheRenderIsTheSame) {
  tt::Run run("print(!\"<\"); print(!\">\"); print(1); print(!\"&\");");
  EXPECT_EQ(run.context.rendered(), "&lt;&gt;1&amp;");
}

TEST(Output, RenderingTwiceGivesTheSameBufferAndEachMustBeFreed) {
  tt::Run run("print(!\"<x>\");");
  char * a = nullptr;
  char * b = nullptr;
  size_t la = 0, lb = 0;
  ASSERT_EQ(gltang_execution_output_render(run.context.execution, &a, &la), GLTANG_OK);
  ASSERT_EQ(gltang_execution_output_render(run.context.execution, &b, &lb), GLTANG_OK);
  EXPECT_NE(a, b);
  EXPECT_STREQ(a, "&lt;x&gt;");
  EXPECT_EQ(la, lb);
  gltang_buffer_free(a);
  gltang_buffer_free(b);
}

TEST(Output, AnEmptyProgramHasAnEmptyOutput) {
  tt::Run run("");
  EXPECT_EQ(run.context.raw(), "");
  EXPECT_EQ(run.context.rendered(), "");
  EXPECT_TRUE(run.context.is_null());
}

// ---------------------------------------------------------------------------
// The resolver seam
// ---------------------------------------------------------------------------

TEST(Resolver, ADottedPathIsGivenWhole) {
  Compiled compiled("use random.global.next_int as n; n;");
  Context context(compiled.program);
  std::vector<std::string> seen;
  gltang_execution_set_resolver(context.execution,
      [](void * user, const char * path, GLTANG_HostValue * out) -> bool {
        static_cast<std::vector<std::string> *>(user)->push_back(path);
        out->kind = GLTANG_HOST_INTEGER;
        out->integer = 41;
        return true;
      },
      &seen);
  ASSERT_TRUE(context.execute());
  ASSERT_EQ(seen.size(), 1u);
  EXPECT_EQ(seen[0], "random.global.next_int");
  EXPECT_EQ(context.integer(), 41);
}

TEST(Resolver, ANameNobodyProvidesIsNullAndNotAnError) {
  tt::Run run("use nothing; use also.nothing as x; [nothing, x];");
  EXPECT_EQ(run.context.describe(), "[null, null]");
}

TEST(Resolver, TheVariableBoundByUseIsOrdinary) {
  Compiled compiled("use a; a = 42; a;");
  Context context(compiled.program);
  context.add_library("a", tt::Host::integer(3));
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(context.integer(), 42);
}

TEST(Resolver, AStringFromTheHostKeepsItsEncodingAndAnInvalidOneIsNull) {
  Compiled compiled("use s; use bad; print(s); [s, bad];");
  Context context(compiled.program);
  context.add_library("s", tt::Host::string("a<b", GLTANG_UNICODE_STRING_TYPE_HTML));
  context.add_library("bad", tt::Host::string(std::string("\xff\xfe", 2)));
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(context.rendered(), "a&lt;b");
  EXPECT_EQ(context.describe(), "[a<b, null]");
}

// ---------------------------------------------------------------------------
// Fuel is charged the same way every time (AD-21)
// ---------------------------------------------------------------------------

TEST(Fuel, TheSameProgramCostsTheSameFuelOnEveryRunAndEveryTier) {
  const char * source = "s = 0; for (i = 0; i < 300; i += 1) { s += i * 2; } print(s);";
  auto cost = [&](bool moving) {
    Compiled compiled(source);
    Context context(compiled.program);
    EXPECT_TRUE(context.ok());
    if (moving) {
      grcore_stack_set_always_move(grcore_context_stack(context.context), true);
    }
    EXPECT_TRUE(context.execute());
    return grcore_context_fuel_used(context.context);
  };
  uint64_t a = cost(false);
  uint64_t b = cost(false);
  uint64_t c = cost(true);
  EXPECT_GT(a, 1000u);
  EXPECT_EQ(a, b);
  EXPECT_EQ(a, c);
}

TEST(Fuel, EveryOpcodeHasACostAndTheTableIsWhatTheInterpreterCharges) {
  for (int op = 0; op < GLTANG_OP_COUNT; ++op) {
    EXPECT_GE(gltang_opcode_cost((GLTANG_Opcode)op), 1u) << gltang_opcode_name((GLTANG_Opcode)op);
    EXPECT_STRNE(gltang_opcode_name((GLTANG_Opcode)op), "?") << op;
  }
  EXPECT_EQ(gltang_opcode_cost(GLTANG_OP_COUNT), 0u);
  EXPECT_STREQ(gltang_opcode_name(GLTANG_OP_COUNT), "?");
}

TEST(Fuel, TheFuelAPausedRunHasUsedIsExactlyWhatWasChargedUpToThePoll) {
  Compiled compiled(RUNAWAY);
  Config config;
  config.fuel = 1000;
  Context context(compiled.program, config);
  ASSERT_FALSE(context.execute());
  uint64_t used = grcore_context_fuel_used(context.context);
  EXPECT_GE(used, 1000u);
  EXPECT_LT(used, 1010u) << "a pause is taken at the first poll after the budget is spent";
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
