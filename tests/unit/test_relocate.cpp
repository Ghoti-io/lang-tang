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


int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
