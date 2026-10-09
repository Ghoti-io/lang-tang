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

#include <memory>

using tt::Compiled;
using tt::Config;
using tt::Context;

namespace {

/// A request that stays pending for the life of the context, so that every
/// runtime poll is a slow one and runs the ACT handlers, the heap's among them:
/// under torture it then collects at the poll, and under relocation moves what
/// it finds. (A fast poll does neither, and only a poll over the memory budget
/// asks the heap to reclaim; this is the narrowest way to make a poll collect
/// without a budget tuned to a program.) The handler votes for nothing.
void idle_poll(GRCORE_Context *, void *, GRCORE_PollCall *) {}
const GRCORE_Key kIdleKey = GRCORE_KEY_INIT("lang-tang relocation test: a request that never ends",
    GRCORE_CARDINALITY_ONE, GRCORE_PHASE_DECIDE, nullptr, idle_poll, nullptr, nullptr, nullptr);

struct SlowPolls {
  GRCORE_Port * port = nullptr;
  explicit SlowPolls(GRCORE_Context * context) {
    GRCORE_RequestKind kind;
    EXPECT_EQ(grcore_context_request_kind(context, &kIdleKey, &kind), GRCORE_OK);
    EXPECT_EQ(grcore_context_port(context, &port), GRCORE_OK);
    EXPECT_EQ(grcore_context_register(context, &kIdleKey, this), GRCORE_OK);
    EXPECT_EQ(grcore_port_post(port, kind), GRCORE_OK);
  }
  ~SlowPolls() { grcore_port_release(port); }
  SlowPolls(const SlowPolls &) = delete;
  SlowPolls & operator=(const SlowPolls &) = delete;
};

struct Outcome {
  bool finished = false;
  std::string described;
  std::string raw;
  size_t errors = 0;
  uint64_t moved = 0;
};

/// Runs `source`, on a relocating heap with a collection at every allocation or
/// on an ordinary one.
Outcome run_program(const std::string & source, bool relocating, bool slow_polls = false) {
  Config config;
  config.relocate = relocating ? 1 : 0;
  config.torture = relocating ? 1 : 0;
  config.verify = relocating ? 1 : 0;
  Compiled compiled(source);
  EXPECT_TRUE(compiled.ok()) << source << ": " << compiled.error.message;
  Context context(compiled.program, config);
  EXPECT_TRUE(context.ok()) << source;
  context.add_native_library();
  std::unique_ptr<SlowPolls> slow;
  if (slow_polls) {
    slow = std::make_unique<SlowPolls>(context.context);
  }
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

void expect_same(const std::string & source, bool slow_polls = false) {
  Outcome plain = run_program(source, false);
  Outcome moved = run_program(source, true, slow_polls);
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

TEST(Relocate, PrintingAndRenderingContainersAndLongStringsGiveTheSameOutput) {
  expect_same("x = [1, \"two\", [3, {a: 4}], 5.5]; print(x); print(\"|\"); print(x[2]); print(\"|\"); print({k: x});");
  expect_same("s = \"y\"; for (i = 0; i < 13; i += 1) { s = s + s; } print(s); print(\"|\"); print([s, [s]]); 1;");
  expect_same("x = [1, [2, [3, [4, [5]]]]]; (x as string) + (x[1] as string);");
}

TEST(Relocate, ANativeBoundToAValueKeepsTheValueItIsBoundTo) {
  expect_same("use random; r = random.seeded(5); f = r.set_seed; f(7); r.next_int;");
  expect_same("use random; r = random.seeded(5); a = r.next_int; r.set_seed(9); (a as string) + (r.next_int as string);");
}

/// The engine's test natives (story 9): one that allocates and reads its argument afterwards (the
/// copy of an argument the shared wrapper pins, planted defect 27), one that re-enters guest code as a
/// nested activation while the references below it are held by the caller's frames, and a chain of
/// frames holding references that a native collects under. Run interpreted, and with every function
/// compiled in the arm's threshold-1 modes, where the native is called from compiled code.
TEST(Relocate, NativesThatAllocateAndReenterReadTheSameWhereverTheObjectsTheyAreGivenMove) {
  expect_same("use alloc_ref; a = [1, 2]; b = alloc_ref(a); c = alloc_ref(b); (c[0][0][1] as string) + (a[0] as string);");
  expect_same(
      "function leaf(x) { use alloc_n; r = alloc_n(5); return x + 1; }\n"
      "use reenter; use alloc_ref; a = [3]; b = alloc_ref(a); (reenter(leaf, 4) as string) + (b[0][0] as string);");
  expect_same(
      "function deep(n, a, b) { use alloc_ref; if (n == 0) { r = alloc_ref(a); k = 0; while (k < 3) { k = k + 1; } return r[0]; } r = deep(n - 1, b, a); return r; }\n"
      "a = [1]; b = [2]; x = deep(30, a, b); (x[0] as string) + (a[0] as string) + (b[0] as string);");
  expect_same(
      "function leaf(x) { use alloc_n; r = alloc_n(4); return x + 1; }\n"
      "function deep(n, a, b) { use reenter; if (n == 0) { t = reenter(leaf, 1); return a; } r = deep(n - 1, b, a); return r; }\n"
      "a = [1]; b = [2]; x = deep(20, a, b); (x[0] as string) + (a[0] as string) + (b[0] as string);");
}

/// With every poll a collecting one, the loops that copy in chunks and poll
/// between chunks (and the sinks) are moved under: each reads its operands again
/// after the poll, or reads poison. Each program is long enough to poll several
/// times (a poll is paid for every 4096 bytes, or its element equivalent).
TEST(Relocate, TheLoopsThatPollAsTheyCopyReadTheSameWhenEveryPollCollectsAndMoves) {
  const char * big = "s = \"h\u00e9llo \"; for (i = 0; i < 12; i += 1) { s = s + s; } ";
  // A concatenation of two long strings, copied in several chunks.
  expect_same(std::string(big) + "t = s + s; (t.length as string) + (t[20000:20010] as string);", true);
  // A slice with a step, copied a grapheme at a time with a poll between.
  expect_same(std::string(big) + "u = s[::3]; (u.length as string) + (u[1000:1010] as string);", true);
  expect_same(std::string(big) + "v = s[100:9000:7]; v as string;", true);
  // A substring and a retag and a render of a long string.
  expect_same(std::string(big) + "w = s[5:30000]; h = w.html; (w.length as string) + (h.render.length as string);", true);
  // An array grown past a poll, and compared and copied at length.
  expect_same("x = [1, 2, 3, 4] * 400; x[2500] = 1; y = x * 1; z = x[10:1500:3]; ((x == y) as string) + (x.size as string) + (z.size as string);", true);
  expect_same("x = [[1, \"k\"], [2, \"k\"]] * 300; y = x + x; ((y == y) as string) + (y.size as string);", true);
  // Printing a long string, and a container of them, into the output.
  expect_same(std::string(big) + "print(s); print(\"|\"); print([s, [s]]); 1;", true);
  expect_same("x = [7, 8, 9, 10] * 700; print(x); y = x as string; y.length;", true);
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

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
