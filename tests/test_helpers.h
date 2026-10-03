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
