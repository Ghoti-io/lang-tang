/**
 * @file
 *
 * Shared helpers for the Ghoti.io Lang-tang unit tests.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GLTANG_TEST_HELPERS_H
#define GHOTI_IO_GLTANG_TEST_HELPERS_H

#include <gtest/gtest.h>

#include <ghoti.io/cutil/memory.h>
#include <ghoti.io/lang-tang/lang-tang.h>
#ifdef GLTANG_WITH_JIT
#include <ghoti.io/runtime-jit/backend.h>
#endif

#include <cstdio>
#include <string>
#include <vector>

#ifndef GLTANG_TEST_DATA
#error "GLTANG_TEST_DATA must name the tests/ directory; the Makefile defines it"
#endif

/* The allocation-failure sweep. Every allocation the library makes by plain
 * malloc, calloc or realloc - its own through cutil's inline gcu_malloc, flex's
 * and bison's - is routed through wrappers (link-time --wrap) that count the
 * call and, when armed, fail exactly the Nth. Disarmed they are pass-throughs. */
namespace alloc_sweep {
/// Fail the Nth wrapped allocation call from now (N from 1); 0 disarms.
void arm(long fail_at);
void disarm();
/// Wrapped calls since the last arm().
long calls();
/// Whether the armed failure was actually delivered.
bool fired();
/// Start (or stop) remembering every block the wrappers hand out, forgetting
/// the ones it has. cutil's counters cannot do this job under failure: a
/// failed gcu_malloc() is still counted as an allocation, so every injected
/// failure would read as a leak.
void track(bool on);
/// Blocks handed out since track(true) and not freed.
size_t live();
/// Prints the surviving blocks (and, with GLTANG_TRACK_STACKS set, who made them).
void dump_live();
/// With GLTANG_TRACK_STACKS set, prints who asked for the allocation that was failed.
void dump_failure();
} // namespace alloc_sweep

/// Whether the wrappers see the allocations the library makes. On Linux cutil's
/// inline gcu_malloc calls malloc, which --wrap reaches. On Windows it calls
/// HeapAlloc, which no link-time wrap can reach, so no allocation can be failed
/// and a sweep over them would measure nothing; those tests skip there.
namespace alloc_sweep {
bool available();
} // namespace alloc_sweep

#define GLTANG_REQUIRE_ALLOC_SWEEP()                                                                  \
  do {                                                                                                \
    if (!alloc_sweep::available()) {                                                                  \
      GTEST_SKIP() << "the allocation-failure sweep cannot reach this platform's allocations "        \
                      "(cutil allocates through HeapAlloc here, which --wrap does not wrap)";         \
    }                                                                                                 \
  } while (0)

/// Whether compiled code can run in this process: the library has the JIT and
/// the target has a native backend for it (runtime-jit has one for Linux x86-64
/// and arm64). Where it cannot, the "JIT arm" of a test is the interpreter, a
/// test of tier-up has nothing to observe and skips, and the checks that the
/// arm "is not vacuous" are not made.
inline bool jit_backend_present() {
#ifdef GLTANG_WITH_JIT
  return grjit_backend_available();
#else
  return false;
#endif
}

#define GLTANG_REQUIRE_JIT_BACKEND()                                                                  \
  do {                                                                                                \
    if (!jit_backend_present()) {                                                                     \
      GTEST_SKIP() << "no native code backend on this target (runtime-jit): nothing tiers up";        \
    }                                                                                                 \
  } while (0)

/// An anonymous scratch file opened "w+b" and deleted when closed. tmpfile()
/// writes to the root of the current drive on Windows, where an ordinary user may
/// not create files, and then returns NULL.
FILE * temp_file();

/// Reads a whole file, bytes and all.
std::string read_file(const std::string & path);

/// A scope in which cutil's allocation counters are zeroed and then compared.
/// `balanced()` is true when every block counted as allocated was freed.
class Balance {
 public:
  Balance() { gcu_memory_reset_counts(); }
  bool balanced() const { return gcu_get_alloc_count() == gcu_get_free_count(); }
  long alloc() const { return (long)gcu_get_alloc_count(); }
  long freed() const { return (long)gcu_get_free_count(); }
};

/// Parse helper: the tree and the result, tree destroyed on scope exit.
struct Parsed {
  GLTANG_Result result = GLTANG_OK;
  GLTANG_ParseError error = {0, 0, {0}};
  GLTANG_Tree * tree = nullptr;
  Parsed(const std::string & source, GLTANG_ParseMode mode) {
    result = gltang_parse(source.c_str(), mode, &error, &tree);
  }
  Parsed(const Parsed &) = delete;
  Parsed & operator=(const Parsed &) = delete;
  ~Parsed() { gltang_tree_destroy(tree); }
};

#endif
