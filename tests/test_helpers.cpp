/**
 * @file
 *
 * Shared helpers for the Ghoti.io Lang-tang unit tests; see test_helpers.h.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include <execinfo.h>
#include <map>
#include <atomic>
#include <unordered_set>

extern "C" {
void * __real_malloc(size_t size);
void * __real_calloc(size_t count, size_t size);
void * __real_realloc(void * pointer, size_t size);
void __real_free(void * pointer);
}

namespace {
// Atomic because the engine tests run contexts on several threads, and every
// one of them goes through these wrappers. Relaxed is enough: the counters
// order nothing, and the sweeps that arm them are single-threaded.
std::atomic<long> g_calls{0};
std::atomic<long> g_fail_at{0};
std::atomic<bool> g_fired{false};
std::atomic<bool> g_tracking{false};
// Blocks handed out by the wrappers while tracking, and not yet freed. The set
// is libstdc++'s, which allocates inside libstdc++ and so does not recurse.
std::unordered_set<void *> & live_blocks() {
  static std::unordered_set<void *> * blocks = new std::unordered_set<void *>();
  return *blocks;
}

// A debugging aid for a failing sweep: with GLTANG_TRACK_STACKS set, the call
// stack that made each tracked block is kept so that dump_live() can say who
// made the survivors.
struct Stack {
  void * frames[24];
  int depth;
};
std::map<void *, Stack> & stacks() {
  static std::map<void *, Stack> * m = new std::map<void *, Stack>();
  return *m;
}
bool want_stacks() {
  static bool want = std::getenv("GLTANG_TRACK_STACKS") != nullptr;
  return want;
}
void remember(void * block) {
  live_blocks().insert(block);
  if (want_stacks()) {
    Stack st;
    st.depth = backtrace(st.frames, 24);
    stacks()[block] = st;
  }
}
void forget(void * block) {
  live_blocks().erase(block);
  if (want_stacks()) {
    stacks().erase(block);
  }
}

void * g_fail_frames[24];
int g_fail_depth = 0;

bool should_fail() {
  long n = ++g_calls;
  long at = g_fail_at;
  if (at != 0 && n == at) {
    g_fired = true;
    if (want_stacks()) {
      g_fail_depth = backtrace(g_fail_frames, 24);
    }
    return true;
  }
  return false;
}
} // namespace

extern "C" {
void * __wrap_malloc(size_t size) {
  if (should_fail()) {
    return nullptr;
  }
  void * block = __real_malloc(size);
  if (g_tracking && block) {
    remember(block);
  }
  return block;
}

void * __wrap_calloc(size_t count, size_t size) {
  if (should_fail()) {
    return nullptr;
  }
  void * block = __real_calloc(count, size);
  if (g_tracking && block) {
    remember(block);
  }
  return block;
}

void * __wrap_realloc(void * pointer, size_t size) {
  if (should_fail()) {
    return nullptr; // the original block is left as it was
  }
  bool was_ours = g_tracking && (pointer == nullptr || live_blocks().count(pointer));
  void * block = __real_realloc(pointer, size);
  if (g_tracking && block) {
    if (pointer && block != pointer) {
      forget(pointer);
    }
    // A block that came from elsewhere (cutil's own storage) and is grown here
    // is not ours to account for: whoever made it frees it.
    if (was_ours) {
      remember(block);
    }
  }
  return block;
}

void __wrap_free(void * pointer) {
  if (g_tracking && pointer) {
    forget(pointer);
  }
  __real_free(pointer);
}
}

namespace alloc_sweep {
void arm(long fail_at) {
  g_calls = 0;
  g_fired = false;
  g_fail_at = fail_at;
}

void disarm() {
  g_fail_at = 0;
}

long calls() {
  return g_calls;
}

bool fired() {
  return g_fired;
}

void track(bool on) {
  g_tracking = on;
  live_blocks().clear();
}

size_t live() {
  return live_blocks().size();
}

void dump_failure() {
  if (g_fail_depth > 0) {
    std::fprintf(stderr, "the failed allocation was asked for by:\n");
    backtrace_symbols_fd(g_fail_frames, g_fail_depth, 2);
  }
}

void dump_live() {
  for (void * block : live_blocks()) {
    std::fprintf(stderr, "live block %p\n", block);
    auto it = stacks().find(block);
    if (it != stacks().end()) {
      backtrace_symbols_fd(it->second.frames, it->second.depth, 2);
    }
  }
}
} // namespace alloc_sweep

std::string read_file(const std::string & path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    ADD_FAILURE() << "cannot read " << path;
    return "";
  }
  std::ostringstream contents;
  contents << file.rdbuf();
  return contents.str();
}
