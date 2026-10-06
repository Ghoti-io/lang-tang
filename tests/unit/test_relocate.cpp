// The engine under runtime-heap's relocation torture (CAP-10): every collection
// moves every unpinned object and poisons the old cell, and a collection runs at
// every allocation (and at a poll that reclaims memory), so a value or a pointer
// into an object that the engine keeps in a C variable across a GC point,
// instead of reading it again from a root, reads poison and fails. (A poll
// collects only when the memory budget asks it to reclaim, so the loops that
// poll as they copy are fixed by the same rule but are reached here only through
// the allocations around them.)
//
// This suite is built only by the relocation arm (`make test-relocate`), which
// links a runtime-heap made with RELOCATE=yes; a normal build has no setter and
// no counters to name. Each test runs a program twice, once on an ordinary heap
// and once on a relocating one, and requires the same result, the same output
// and the same error list, and that the relocating heap really did move
// objects. The programs are the shapes that held a value across an allocation or
// a poll: growing an array or a map, copying and comparing containers, the
// operators that build arrays, every string operation that copies in chunks,
// printing and rendering containers, and a native bound to a value.

#include "exec_harness.h"

using tt::Compiled;
using tt::Config;
using tt::Context;

namespace {

struct Outcome {
  bool finished = false;
  std::string described;
  std::string raw;
  size_t errors = 0;
  uint64_t moved = 0;
};

/// Runs `source`, on a relocating heap with a collection at every allocation or
/// on an ordinary one.
Outcome run_program(const std::string & source, bool relocating) {
  Config config;
  config.relocate = relocating ? 1 : 0;
  config.torture = relocating ? 1 : 0;
  config.verify = relocating ? 1 : 0;
  Compiled compiled(source);
  EXPECT_TRUE(compiled.ok()) << source << ": " << compiled.error.message;
  Context context(compiled.program, config);
  EXPECT_TRUE(context.ok()) << source;
  Outcome out;
  out.finished = context.execute();
  out.described = out.finished ? context.describe() : std::string("<no result>");
  out.raw = context.raw();
  out.errors = context.error_count();
  GRHEAP_RelocationStats stats = {};
  EXPECT_EQ(grheap_relocation_stats(context.heap, &stats), GRHEAP_OK);
  out.moved = stats.moved;
  return out;
}

void expect_same(const std::string & source) {
  Outcome plain = run_program(source, false);
  Outcome moved = run_program(source, true);
  EXPECT_EQ(plain.moved, 0u) << "the ordinary heap moved something: " << source;
  EXPECT_GT(moved.moved, 0u) << "the relocating heap moved nothing, so this proves nothing: " << source;
  EXPECT_EQ(moved.finished, plain.finished) << source;
  EXPECT_EQ(moved.described, plain.described) << source;
  EXPECT_EQ(moved.raw, plain.raw) << source;
  EXPECT_EQ(moved.errors, plain.errors) << source;
}

}  // namespace

TEST(Relocate, ArraysAndMapsThatGrowReadTheSameWhereverTheirStorageMoves) {
  expect_same("x = [1, 2]; x[5] = 7; x as string;");
  expect_same("m = {:}; for (i = 0; i < 40; i += 1) { m[\"k\" + (i as string)] = [i, i * 2]; } m as string;");
  expect_same("m = {a: 1, b: 2, c: 3, d: 4}; m.e = [5]; m.f = {g: 6}; m as string;");
}

TEST(Relocate, AValueStoredBeyondTheEndOfTheArrayItIsGrownFromIsStillTheValueAfterTheGrowth) {
  // The value is the array itself: it is moved by the growth that makes room.
  expect_same("a = [1]; a[5] = a; a as string;");
  expect_same("a = [1]; a[2] = a; a as string;");
  // And a boxed integer, which is a heap object of its own.
  expect_same("use random; r = random.seeded(123); xs = []; for (i = 0; i < 40; i += 1) { xs[i] = r.next_int; } xs as string;");
}

TEST(Relocate, AContainerStoredIntoItselfOrIntoAMapIsCopiedAndTheCopyIsWhatIsStored) {
  expect_same("x = [1, 2]; x[0] = x; x as string;");
  expect_same("m = {:}; m[\"k\"] = m; m as string;");
  expect_same("d = {a: 1}; d.b = d; d.b.a;");
  expect_same("d = {a: 1}; d.list = [1, [2, 3], {k: [4]}]; d.list[1][0] = 9; d as string;");
}

TEST(Relocate, TheRunIsDrivenByTheRelocatingHeapAndNotByAnOrdinaryOneThatLooksTheSame) {
  // The controls: the ordinary heap really does not move, and the relocating one
  // does, for the smallest program that allocates twice.
  Outcome plain = run_program("x = [1, 2, 3]; x[7] = x; x as string;", false);
  Outcome moved = run_program("x = [1, 2, 3]; x[7] = x; x as string;", true);
  EXPECT_EQ(plain.moved, 0u);
  EXPECT_GT(moved.moved, 0u);
  EXPECT_EQ(plain.described, moved.described);
}
TEST(Relocate, CopyingAndComparingNestedContainersGivesTheSameAnswersWhereverTheyMove) {
  expect_same("x = [[1, [2, 3]], {k: [4, 5]}, \"s\"]; y = x; x[0][1][0] = 9; ((x == y) as string) + (y as string);");
  expect_same("x = [[1, [2, 3]], [4]]; y = [[1, [2, 3]], [4]]; (x == y) as string;");
  expect_same("x = [[1, [2, 3]], [4]]; y = [[1, [2, 4]], [4]]; (x == y) as string;");
}

TEST(Relocate, TheOperatorsThatBuildArraysFromArraysGiveTheSameArrays) {
  expect_same("x = [[1], [2, 3], \"s\"]; y = x + x; z = x * 3; w = z[1:7]; (y as string) + (z as string) + (w as string);");
  expect_same("x = [1, [2, 3], 4, [5], 6, 7]; (x[::2] as string) + (x[1:5] as string) + (x[::-1] as string);");
}

TEST(Relocate, EveryStringOperationThatCopiesInChunksReadsTheSameWhereverTheStringMoves) {
  // Long enough to be copied in several chunks, with graphemes of several
  // widths so there is an offsets table to copy too.
  expect_same("s = \"héllo wörld \"; for (i = 0; i < 10; i += 1) { s = s + s; } t = s + s; (t[3:9000] as string) + (t[::7] as string);");
  expect_same("s = \"x\"; for (i = 0; i < 14; i += 1) { s = s + s; } t = s + s; (t.length as string) + (t[100:9000].length as string) + (t[::2].length as string);");
  expect_same("s = \"a<b>&c \"; for (i = 0; i < 12; i += 1) { s = s + s; } h = s.html; (h + s + 12 + [1, \"x\"]) as string;");
  expect_same("s = \"a<b>&c\"; h = s.html; r = h.render; (r + (h as string) + (s.raw as string)) as string;");
  expect_same("s = \"abc\"; (s + 1) + (2 + s) + ([1, \"x\"] as string) + s + true;");
}


int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
