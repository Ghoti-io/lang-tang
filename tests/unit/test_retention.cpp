// The heap's retention query (runtime-heap's retention.h) on a real engine:
// why is this object of a paused Tang program still alive?
//
// A function builds a string at run time and puts it in an array, keeps a local
// variable for the string, and loops. (A string is shared by reference when it is
// stored, as an immutable value; an array stored in an array would be copied, and
// the copy is the thing the outer array holds.) The run is paused by fuel in the
// loop and the string's address is read from the frame's variable. The
// local is then set to null and the run goes on to a second pause; from then
// on the only thing keeping the inner array alive is the outer array's storage,
// which a local variable holds. The query must say exactly that: the guest
// stack's root source, then the outer array, its storage, and the inner array.
// It must say the same with every function tiering up at its first poll (a pause
// at a compiled poll leaves a frame the interpreter can read: the frame is
// written back before the poll, AD-17).

#include "exec_harness.h"
#include "test_helpers.h"

#include <ghoti.io/runtime-core/runtime-core.h>
#include <ghoti.io/runtime-heap/runtime-heap.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using tt::Compiled;
using tt::Config;
using tt::Context;
using tt::Mode;

namespace {

const char * const kProgram =
    "function work() {\n"             // 1
    "  s = \"leak\";\n"                // 2
    "  inner = s + s + s;\n"          // 3 (a string built at run time)
    "  outer = [inner];\n"            // 4
    "  i = 0;\n"                      // 4
    "  while (i < 3000) {\n"          // 5
    "    i = i + 1;\n"                // 6
    "  }\n"                           // 7
    "  inner = null;\n"               // 8
    "  j = 0;\n"                      // 9
    "  while (j < 3000) {\n"          // 10
    "    j = j + 1;\n"                // 11
    "  }\n"                           // 12
    "  return outer;\n"               // 13
    "}\n"                             // 14
    "work();\n";                      // 15

/// The value word of the local variable `name` of the innermost frame that has
/// one, found through the frame's scope interface.
bool find_local(GRCORE_Context * context, const char * name, uint64_t * word) {
  GRCORE_FrameWalk walk;
  GRCORE_AbstractFrame frame;
  if (grcore_frame_walk_begin(context, &walk) != GRCORE_OK) {
    return false;
  }
  while (grcore_frame_walk_next(&walk, &frame)) {
    GRCORE_ScopeInfo scope;
    if (grcore_frame_scope_count(&frame) == 0 || grcore_frame_scope(&frame, 0, &scope) != GRCORE_OK ||
        scope.kind != GRCORE_SCOPE_LOCAL) {
      continue;
    }
    for (size_t i = 0; i < scope.variable_count; i++) {
      GRCORE_Variable v;
      if (grcore_frame_variable(&frame, 0, i, &v) == GRCORE_OK && std::strcmp(v.name, name) == 0) {
        *word = v.value;
        return true;
      }
    }
  }
  return false;
}

/// A pointer value (tag 0) as a heap address, or null for any other word.
void * as_pointer(uint64_t word) {
  return (word & 0xF) == 0 ? reinterpret_cast<void *>(word) : nullptr;
}

struct Chain {
  bool retained = false;
  GRHEAP_RetentionRoot root{};
  std::vector<std::string> types;
  std::vector<size_t> offsets;
  bool ok = false;
};

Chain ask(GRHEAP_Heap * heap, void * object) {
  Chain c;
  GRHEAP_Retention * path = nullptr;
  GRHEAP_Result r = grheap_retention_path(heap, object, nullptr, &path);
  EXPECT_EQ(r, GRHEAP_OK) << grheap_result_string(r);
  if (r != GRHEAP_OK) {
    return c;
  }
  c.ok = true;
  c.retained = grheap_retention_retained(path);
  if (c.retained) {
    EXPECT_EQ(grheap_retention_root(path, &c.root), GRHEAP_OK);
    for (size_t i = 0; i < grheap_retention_step_count(path); i++) {
      GRHEAP_RetentionStep s;
      EXPECT_EQ(grheap_retention_step(path, i, &s), GRHEAP_OK);
      c.types.push_back(s.type->name != nullptr ? s.type->name : "?");
      c.offsets.push_back(s.slot_offset);
      EXPECT_EQ(s.target, i + 1 == grheap_retention_step_count(path));
    }
  }
  grheap_retention_release(path);
  return c;
}

/// Runs the program to its first pause, reads the inner array, runs on to the
/// second pause (where the local is null) and asks the query about it.
void leaked_element(long threshold) {
  Compiled page(kProgram, Mode::Script, "leak.tang");
  ASSERT_TRUE(page.ok()) << page.error.message;
  Config config;
  config.fuel = 600;  // inside the first loop
  config.jit_threshold = threshold;
  Context context(page.program, config);
  ASSERT_TRUE(context.ok());
  EXPECT_FALSE(context.execute());
  ASSERT_TRUE(context.paused());
#ifdef GLTANG_WITH_JIT
  if (threshold == 1) {
    EXPECT_GE(context.jit_stats().functions_compiled, 1u) << "the JIT arm must not be vacuous";
  }
#endif

  // Phase one: the local holds the inner array, so a path of one step from the
  // frame's own slot (the variable) to it is found.
  uint64_t inner_word = 0, outer_word = 0;
  ASSERT_TRUE(find_local(context.context, "inner", &inner_word));
  ASSERT_TRUE(find_local(context.context, "outer", &outer_word));
  void * inner = as_pointer(inner_word);
  void * outer = as_pointer(outer_word);
  ASSERT_NE(inner, nullptr) << std::hex << inner_word;
  ASSERT_NE(outer, nullptr) << std::hex << outer_word;
  ASSERT_TRUE(grheap_contains(context.heap, inner));
  Chain held = ask(context.heap, inner);
  ASSERT_TRUE(held.retained);
  EXPECT_STREQ(held.root.source, "runtime-core.guest");
  EXPECT_EQ(held.root.kind, GRHEAP_ROOT_CONTEXT_SOURCE);
  // Held by the variable directly (a one-step chain) or, equally short, by
  // nothing longer: never through the outer array when a shorter chain exists.
  ASSERT_EQ(held.types.size(), 1u) << "the local variable holds it";
  EXPECT_EQ(held.types[0], "lang-tang string");

  // Phase two: run on until the local is null.
  uint64_t guard = 0;
  for (;;) {
    ASSERT_TRUE(find_local(context.context, "inner", &inner_word));
    if (as_pointer(inner_word) == nullptr || inner_word == 0) {
      break;
    }
    ASSERT_LT(++guard, 100000u);
    grcore_context_set_fuel(context.context, grcore_context_fuel_used(context.context) + 300);
    context.resume();
    ASSERT_TRUE(context.paused()) << "the run ended before the local was cleared";
  }
  // The local no longer names it; the outer array still does.
  EXPECT_TRUE(grheap_contains(context.heap, inner)) << "unswept: still a live payload";
  Chain leaked = ask(context.heap, inner);
  ASSERT_TRUE(leaked.retained);
  EXPECT_EQ(leaked.root.kind, GRHEAP_ROOT_CONTEXT_SOURCE);
  EXPECT_STREQ(leaked.root.source, "runtime-core.guest");
  ASSERT_EQ(leaked.types.size(), 3u) << "outer array, its storage, the string";
  EXPECT_EQ(leaked.types[0], "lang-tang array");
  EXPECT_EQ(leaked.types[1], "lang-tang array storage");
  EXPECT_EQ(leaked.types[2], "lang-tang string");
  // The array object keeps its storage at byte 16 (kind, reserved, length), and
  // the storage keeps its first element at byte 16 (kind, reserved, capacity).
  EXPECT_EQ(leaked.offsets[0], 16u);
  EXPECT_EQ(leaked.offsets[1], 16u);

  // The collector agrees: a collection keeps it.
  ASSERT_EQ(grheap_collect(context.heap), GRHEAP_OK);
  EXPECT_TRUE(grheap_contains(context.heap, inner));
  // And finishing the run, the outer array is the result and still holds it.
  EXPECT_TRUE(context.finished_after_raising(10000000));
}

}  // namespace

TEST(Retention, ALeakedArrayElementIsExplainedThroughTheGuestStackAndTheOuterArray) {
  leaked_element(0);
}

TEST(Retention, TheSameWithEveryFunctionTieringUpAtItsFirstPoll) {
  leaked_element(1);
}

TEST(Retention, AnArrayNothingReachesAnyMoreIsNotRetainedAndTheCollectorFreesIt) {
  const char * source =
      "function work() {\n"
      "  s = \"leak\";\n"
      "  inner = s + s + s;\n"
      "  i = 0;\n"
      "  while (i < 3000) {\n"
      "    i = i + 1;\n"
      "  }\n"
      "  inner = null;\n"
      "  j = 0;\n"
      "  while (j < 3000) {\n"
      "    j = j + 1;\n"
      "  }\n"
      "  return 1;\n"
      "}\n"
      "work();\n";
  for (long threshold : {0L, 1L}) {
    Compiled page(source, Mode::Script, "gone.tang");
    ASSERT_TRUE(page.ok());
    Config config;
    config.fuel = 600;
    config.jit_threshold = threshold;
    Context context(page.program, config);
    ASSERT_TRUE(context.ok());
    EXPECT_FALSE(context.execute());
    ASSERT_TRUE(context.paused());
    uint64_t word = 0;
    ASSERT_TRUE(find_local(context.context, "inner", &word));
    void * inner = as_pointer(word);
    ASSERT_NE(inner, nullptr);
    for (uint64_t guard = 0;; guard++) {
      ASSERT_TRUE(find_local(context.context, "inner", &word));
      if (as_pointer(word) == nullptr || word == 0) {
        break;
      }
      ASSERT_LT(guard, 100000u);
      grcore_context_set_fuel(context.context, grcore_context_fuel_used(context.context) + 300);
      context.resume();
      ASSERT_TRUE(context.paused());
    }
    if (!grheap_contains(context.heap, inner)) {
      // Under GC torture a collection runs at every GC point, so the string is
      // already freed by the time the run is paused: the collector has answered
      // the question, and the query would (rightly) refuse a freed address.
      EXPECT_TRUE(tt::torture_requested());
      EXPECT_TRUE(context.finished_after_raising(10000000));
      continue;
    }
    Chain c = ask(context.heap, inner);
    ASSERT_TRUE(c.ok);
    EXPECT_FALSE(c.retained) << "the variable was cleared and nothing else holds the array";
    ASSERT_EQ(grheap_collect(context.heap), GRHEAP_OK);
    EXPECT_FALSE(grheap_contains(context.heap, inner)) << "the collector then frees it, as the query said";
    EXPECT_TRUE(context.finished_after_raising(10000000));
  }
}

TEST(Retention, TheQueryChangesNothingTheNextCollectionOrTheResultCouldSee) {
  Compiled page(kProgram, Mode::Script, "same.tang");
  ASSERT_TRUE(page.ok());
  Config config;
  config.fuel = 600;
  Context context(page.program, config);
  ASSERT_TRUE(context.ok());
  EXPECT_FALSE(context.execute());
  ASSERT_TRUE(context.paused());
  uint64_t word = 0;
  ASSERT_TRUE(find_local(context.context, "outer", &word));
  void * outer = as_pointer(word);
  ASSERT_NE(outer, nullptr);
  GRHEAP_Stats before{};
  ASSERT_EQ(grheap_stats(context.heap, &before), GRHEAP_OK);
  for (int i = 0; i < 20; i++) {
    Chain c = ask(context.heap, outer);
    ASSERT_TRUE(c.retained);
  }
  GRHEAP_Stats after{};
  ASSERT_EQ(grheap_stats(context.heap, &after), GRHEAP_OK);
  EXPECT_EQ(std::memcmp(&before, &after, sizeof before), 0);
  EXPECT_TRUE(context.finished_after_raising(10000000));
  EXPECT_TRUE(context.is_array());
  EXPECT_EQ(context.size(), 1u);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
