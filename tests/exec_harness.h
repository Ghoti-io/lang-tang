/**
 * @file
 *
 * A host for the execution tests: the group, the options, the context, the
 * heap and the execution that a host makes with runtime-core and runtime-heap,
 * wrapped so that a test says what it runs and what it expects.
 *
 * Every allocation the context, the heap and the engine make passes through a
 * tracker that counts live blocks and live pages, so each test also proves that
 * destroying the context gives all of it back (nothing leaks). The tracker can
 * fail the Nth allocation, which is how the allocation-failure sweep reaches
 * the engine's own arms.
 *
 * The variants the suite is run under are chosen by the environment so that
 * one binary serves them all: GRHEAP_TORTURE=1 and GRHEAP_VERIFY=1 are the
 * heap's own (runtime-heap reads them), and GLTANG_TEST_MOVING_STACK=1 moves
 * the guest stack on every push.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GLTANG_TEST_EXEC_HARNESS_H
#define GHOTI_IO_GLTANG_TEST_EXEC_HARNESS_H

#include <gtest/gtest.h>

#include <ghoti.io/cutil/memory.h>
#include <ghoti.io/lang-tang/compile.h>
#include <ghoti.io/lang-tang/execution.h>
#include <ghoti.io/lang-tang/lang-tang.h>
#include <ghoti.io/lang-tang/library.h>
#include <ghoti.io/lang-tang/seeds.h>
#include <ghoti.io/runtime-core/runtime-core.h>
#include <ghoti.io/runtime-heap/runtime-heap.h>

#include <malloc.h>

#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace tt {

// ---------------------------------------------------------------------------
// The tracker
// ---------------------------------------------------------------------------

/// Counts the live blocks and pages a group hands out, and fails the Nth.
struct Tracker {
  long live_blocks = 0;
  long live_pages = 0;
  long calls = 0;
  long fail_at = 0;     ///< 0: never.
  bool fired = false;
  // The bytes the group holds from the allocator and from the page provider,
  // now and at the most. The native gate reads the peak as the work a run did
  // with memory, a counter the test owns.
  size_t live_bytes = 0;
  size_t peak_bytes = 0;
  GRCORE_Allocator allocator;
  GRCORE_PageProvider pages;
  const GRCORE_PageProvider * base_pages = grcore_page_provider_default();

  Tracker() {
    allocator.ctx = this;
    allocator.malloc_fn = &Tracker::do_malloc;
    allocator.calloc_fn = &Tracker::do_calloc;
    allocator.realloc_fn = &Tracker::do_realloc;
    allocator.free_fn = &Tracker::do_free;
    pages.ctx = this;
    pages.page_size = base_pages->page_size;
    pages.map = &Tracker::do_map;
    pages.unmap = &Tracker::do_unmap;
  }

  bool should_fail() {
    ++calls;
    if (fail_at != 0 && calls == fail_at) {
      fired = true;
      return true;
    }
    return false;
  }

  void grew(size_t bytes) {
    live_bytes += bytes;
    if (live_bytes > peak_bytes) {
      peak_bytes = live_bytes;
    }
  }

  static void * do_malloc(void * ctx, size_t size) {
    Tracker * t = static_cast<Tracker *>(ctx);
    if (t->should_fail()) {
      return nullptr;
    }
    void * p = std::malloc(size ? size : 1);
    t->live_blocks += p != nullptr;
    if (p) {
      t->grew(malloc_usable_size(p));
    }
    return p;
  }

  static void * do_calloc(void * ctx, size_t n, size_t size) {
    Tracker * t = static_cast<Tracker *>(ctx);
    if (t->should_fail()) {
      return nullptr;
    }
    void * p = std::calloc(n ? n : 1, size ? size : 1);
    t->live_blocks += p != nullptr;
    if (p) {
      t->grew(malloc_usable_size(p));
    }
    return p;
  }

  static void * do_realloc(void * ctx, void * old, size_t size) {
    Tracker * t = static_cast<Tracker *>(ctx);
    if (t->should_fail()) {
      return nullptr;
    }
    size_t before = old ? malloc_usable_size(old) : 0;
    void * p = std::realloc(old, size ? size : 1);
    if (p && !old) {
      ++t->live_blocks;
    }
    if (p) {
      size_t after = malloc_usable_size(p);
      t->live_bytes -= before;
      t->grew(after);
    }
    return p;
  }

  static void do_free(void * ctx, void * p) {
    if (p) {
      Tracker * t = static_cast<Tracker *>(ctx);
      --t->live_blocks;
      t->live_bytes -= malloc_usable_size(p);
      std::free(p);
    }
  }

  static void * do_map(void * ctx, size_t size) {
    Tracker * t = static_cast<Tracker *>(ctx);
    if (t->should_fail()) {
      return nullptr;
    }
    void * p = t->base_pages->map(t->base_pages->ctx, size);
    t->live_pages += p != nullptr;
    if (p) {
      t->grew(size);
    }
    return p;
  }

  static void do_unmap(void * ctx, void * p, size_t size) {
    Tracker * t = static_cast<Tracker *>(ctx);
    --t->live_pages;
    t->live_bytes -= size;
    t->base_pages->unmap(t->base_pages->ctx, p, size);
  }
};

// ---------------------------------------------------------------------------
// Host values
// ---------------------------------------------------------------------------

/// A value a test hands the engine through `use`.
struct Host {
  GLTANG_HostValue value;
  std::string text;

  Host() { std::memset(&value, 0, sizeof(value)); }
  static Host null() { return Host(); }
  static Host boolean(bool b) {
    Host h;
    h.value.kind = GLTANG_HOST_BOOL;
    h.value.boolean = b;
    return h;
  }
  static Host integer(int64_t n) {
    Host h;
    h.value.kind = GLTANG_HOST_INTEGER;
    h.value.integer = n;
    return h;
  }
  static Host number(double d) {
    Host h;
    h.value.kind = GLTANG_HOST_FLOAT;
    h.value.number = d;
    return h;
  }
  static Host string(const std::string & s, GLTANG_String_Type type = GLTANG_UNICODE_STRING_TYPE_TRUSTED) {
    Host h;
    h.text = s;
    h.value.kind = GLTANG_HOST_STRING;
    h.value.encoding = type;
    return h;
  }
};

// ---------------------------------------------------------------------------
// A compiled program
// ---------------------------------------------------------------------------

enum class Mode { Script, Template };

class Compiled {
 public:
  GLTANG_Result result = GLTANG_ERR_INTERNAL;
  GLTANG_ParseError error = {0, 0, {0}};
  GLTANG_Program * program = nullptr;

  Compiled(const std::string & source, Mode mode = Mode::Script, const char * file = "test.tang") {
    GLTANG_Tree * tree = nullptr;
    result = gltang_parse(source.c_str(), mode == Mode::Script ? GLTANG_PARSE_SCRIPT : GLTANG_PARSE_TEMPLATE, &error, &tree);
    if (result == GLTANG_OK) {
      result = gltang_compile(tree, file, &error, &program);
    }
    gltang_tree_destroy(tree);
  }
  Compiled(const Compiled &) = delete;
  Compiled & operator=(const Compiled &) = delete;
  ~Compiled() { gltang_program_release(program); }
  bool ok() const { return result == GLTANG_OK && program != nullptr; }
};

// ---------------------------------------------------------------------------
// A context running a program
// ---------------------------------------------------------------------------

struct Config {
  uint64_t fuel = GRCORE_UNLIMITED;
  uint64_t memory_bytes = GRCORE_UNLIMITED;
  uint64_t memory_reserve = GRCORE_DEFAULT_MEMORY_RESERVE;
  uint64_t calls = 512;               ///< How many calls may nest (ctang's max_call_depth); GRCORE_UNLIMITED for none.
  uint64_t gc_threshold = GRHEAP_DEFAULT_GC_THRESHOLD;
  bool arena = false;
  long fail_at = 0;                   ///< Fail the Nth allocation of the run (0: never).
  // The instruments, each three-way: -1 takes the environment's choice
  // (GRHEAP_TORTURE, GRHEAP_VERIFY, GLTANG_TEST_MOVING_STACK, which is how a
  // whole suite is run under them), 0 forces it off and 1 forces it on. The
  // observer runs one program under several settings in one process.
  int torture = -1;
  int verify = -1;
  int moving_stack = -1;
};

inline bool moving_stack_requested() {
  const char * v = std::getenv("GLTANG_TEST_MOVING_STACK");
  return v && *v && std::strcmp(v, "0") != 0;
}

/// An element or member of the result, read as plain data.
struct Item {
  GLTANG_ResultItem item;
  bool present = false;
  bool is_null() const { return present && item.kind == GLTANG_KIND_NULL; }
  bool is_bool() const { return present && item.kind == GLTANG_KIND_BOOL; }
  bool is_integer() const { return present && item.kind == GLTANG_KIND_INTEGER; }
  bool is_float() const { return present && item.kind == GLTANG_KIND_FLOAT; }
  bool is_string() const { return present && item.kind == GLTANG_KIND_STRING; }
  bool is_array() const { return present && item.kind == GLTANG_KIND_ARRAY; }
  bool is_map() const { return present && item.kind == GLTANG_KIND_MAP; }
  bool is_error() const { return present && item.kind == GLTANG_KIND_ERROR; }
  bool boolean() const { return item.boolean; }
  int64_t integer() const { return item.integer; }
  double number() const { return item.number; }
  size_t size() const { return item.size; }
  std::string text() const { return item.text ? std::string(item.text, item.length) : std::string(); }
};

class Context {
 public:
  Tracker tracker;
  GRCORE_Group * group = nullptr;
  GRCORE_Context * context = nullptr;
  GRHEAP_Heap * heap = nullptr;
  GLTANG_Execution * execution = nullptr;
  GLTANG_Result created = GLTANG_ERR_INTERNAL;
  GRCORE_Result ran = GRCORE_ERR_INTERNAL;
  GRCORE_Outcome outcome = GRCORE_OUTCOME_FINISHED;
  bool has_run = false;
  bool arena = false;  ///< Arena mode never collects, so torture has nothing to count.
  bool torture_on = false;  ///< Whether this context's heap was made with torture on.
  GLTANG_Library * root = nullptr;   ///< The execution's library, attached when the run starts.
  bool attached = false;

  explicit Context(GLTANG_Program * program, const Config & config = Config()) {
    tracker.fail_at = config.fail_at;
    arena = config.arena;
    GRCORE_Options * options = nullptr;
    GRHEAP_Options * heap_options = nullptr;
    do {
      if (grcore_group_create(&tracker.allocator, &tracker.pages, &group) != GRCORE_OK) {
        created = GLTANG_ERR_OOM;
        break;
      }
      if (grcore_options_create(&tracker.allocator, &options) != GRCORE_OK) {
        created = GLTANG_ERR_OOM;
        break;
      }
      grcore_options_set_fuel(options, config.fuel);
      grcore_options_set_memory_bytes(options, config.memory_bytes);
      grcore_options_set_memory_reserve(options, config.memory_reserve);
      grcore_options_set_guest_depth(options, config.calls == GRCORE_UNLIMITED ? config.calls : config.calls + 1u);
      if (grcore_context_create(group, options, &context) != GRCORE_OK) {
        created = GLTANG_ERR_OOM;
        break;
      }
      if (grheap_options_create(&tracker.allocator, &heap_options) != GRHEAP_OK) {
        created = GLTANG_ERR_OOM;
        break;
      }
      gltang_heap_options_configure(heap_options);
      grheap_options_set_gc_threshold(heap_options, config.gc_threshold);
      if (config.torture >= 0) {
        grheap_options_set_torture(heap_options, config.torture != 0);
      }
      if (config.verify >= 0) {
        grheap_options_set_verify(heap_options, config.verify != 0 ? GRHEAP_VERIFY_ABORT : GRHEAP_VERIFY_OFF);
      }
      torture_on = grheap_options_get_torture(heap_options);
      if (config.arena) {
        grheap_options_set_mode(heap_options, GRHEAP_MODE_ARENA);
      }
      if (grheap_heap_create(context, heap_options, &heap) != GRHEAP_OK) {
        created = GLTANG_ERR_OOM;
        break;
      }
      created = gltang_execution_create(context, program, &execution);
      if (created != GLTANG_OK) {
        break;
      }
      if (config.moving_stack > 0 || (config.moving_stack < 0 && moving_stack_requested())) {
        grcore_stack_set_always_move(grcore_context_stack(context), true);
      }
    } while (false);
    grheap_options_destroy(heap_options);
    grcore_options_destroy(options);
  }

  Context(const Context &) = delete;
  Context & operator=(const Context &) = delete;

  ~Context() {
    destroy();
  }

  /// Destroys the context and the group, and checks that nothing is left.
  void destroy() {
    gltang_library_release(root);
    root = nullptr;
    if (context && heap) {
      GRHEAP_Stats stats;
      if (grheap_stats(heap, &stats) == GRHEAP_OK) {
        EXPECT_EQ(stats.verify_violations, 0u) << "a pointer was written into a heap object without grheap_store";
        if (!arena && torture_on && stats.allocations > 1) {
          // The instrument is on only if it ran: a torture suite that never
          // collected would pass for the wrong reason.
          EXPECT_GT(stats.torture_collections, 0u);
        }
      }
    }
    if (context) {
      EXPECT_EQ(grcore_context_destroy(context), GRCORE_OK);
      context = nullptr;
    }
    if (group) {
      EXPECT_EQ(grcore_group_destroy(group), GRCORE_OK);
      group = nullptr;
    }
    EXPECT_EQ(tracker.live_blocks, 0) << "blocks the group handed out and never got back";
    EXPECT_EQ(tracker.live_pages, 0) << "pages the group handed out and never got back";
  }

  bool ok() const { return created == GLTANG_OK && execution != nullptr; }

  /// The execution's unnamed library, made on first use and attached by the
  /// first run. A test adds natives, templates and sub-libraries to it.
  GLTANG_Library * library() {
    if (!root && gltang_library_create(nullptr, &root) != GLTANG_OK) {
      root = nullptr;
    }
    return root;
  }

  /// A value `use` finds by name.
  bool add_library(const std::string & name, const Host & value) {
    GLTANG_Library * lib = library();
    if (!lib) {
      return false;
    }
    GLTANG_Result r = GLTANG_ERR_INVALID;
    switch (value.value.kind) {
      case GLTANG_HOST_NULL: r = gltang_library_add_null(lib, name.c_str()); break;
      case GLTANG_HOST_BOOL: r = gltang_library_add_bool(lib, name.c_str(), value.value.boolean); break;
      case GLTANG_HOST_INTEGER: r = gltang_library_add_integer(lib, name.c_str(), value.value.integer); break;
      case GLTANG_HOST_FLOAT: r = gltang_library_add_float(lib, name.c_str(), value.value.number); break;
      case GLTANG_HOST_STRING: r = gltang_library_add_string(lib, name.c_str(), value.text.data(), value.text.size(), value.value.encoding); break;
    }
    return r == GLTANG_OK;
  }

  /// Attaches the library, once, before the run begins.
  void attach() {
    if (!attached && root && execution) {
      EXPECT_EQ(gltang_execution_set_libraries(execution, root), GLTANG_OK);
      attached = true;
    }
  }

  /// Runs to the end. Returns true if the program finished.
  bool execute() {
    attach();
    has_run = true;
    ran = grcore_run(context, gltang_execution_entry, execution, &outcome);
    return ran == GRCORE_OK && outcome == GRCORE_OUTCOME_FINISHED;
  }

  /// Resumes a paused run.
  bool resume() {
    ran = grcore_resume(context, &outcome);
    return ran == GRCORE_OK && outcome == GRCORE_OUTCOME_FINISHED;
  }

  /// Gives a paused run `more` fuel beyond what it has used and resumes it.
  bool finished_after_raising(uint64_t more) {
    grcore_context_set_fuel(context, grcore_context_fuel_used(context) + more);
    return resume();
  }

  bool paused() const { return ran == GRCORE_OK && outcome == GRCORE_OUTCOME_PAUSED; }

  // The result, as the kinds a host reads.
  GLTANG_ValueKind kind() const { return gltang_execution_result_kind(execution); }
  bool is_null() const { return kind() == GLTANG_KIND_NULL; }
  bool is_bool() const { return kind() == GLTANG_KIND_BOOL; }
  bool is_integer() const { return kind() == GLTANG_KIND_INTEGER; }
  bool is_float() const { return kind() == GLTANG_KIND_FLOAT; }
  bool is_string() const { return kind() == GLTANG_KIND_STRING; }
  bool is_array() const { return kind() == GLTANG_KIND_ARRAY; }
  bool is_map() const { return kind() == GLTANG_KIND_MAP; }
  bool is_error() const { return kind() == GLTANG_KIND_ERROR; }
  bool is_function() const { return kind() == GLTANG_KIND_FUNCTION; }
  bool boolean() const { return gltang_execution_result_bool(execution); }
  int64_t integer() const { return gltang_execution_result_integer(execution); }
  double number() const { return gltang_execution_result_float(execution); }
  size_t size() const { return gltang_execution_result_size(execution); }
  std::string text() const {
    size_t length = 0;
    const char * t = gltang_execution_result_text(execution, &length);
    return t ? std::string(t, length) : std::string();
  }
  /// The result as `as string` shows it (and `Error: ...` for an error).
  std::string describe() const {
    char * d = nullptr;
    size_t length = 0;
    if (gltang_execution_result_describe(execution, &d, &length) != GLTANG_OK) {
      return "<describe failed>";
    }
    std::string s(d, length);
    gltang_buffer_free(d);
    return s;
  }
  GLTANG_ErrorKind error_kind() const {
    GLTANG_ErrorKind kind = GLTANG_ERROR_KIND_COUNT;
    gltang_execution_result_error(execution, &kind, nullptr);
    return kind;
  }
  GLTANG_ErrorOrigin error_origin() const {
    GLTANG_ErrorOrigin origin = {nullptr, 0, 0, 0};
    gltang_execution_result_error(execution, nullptr, &origin);
    return origin;
  }

  Item element(size_t index) const {
    Item i;
    i.present = gltang_execution_result_element(execution, index, &i.item);
    return i;
  }
  Item member(const char * key) const {
    Item i;
    i.present = gltang_execution_result_member(execution, key, &i.item);
    return i;
  }

  /// An entry of the error list, with its chain.
  struct Entry {
    GLTANG_ErrorEntry e;
    std::vector<GLTANG_ErrorLink> chain;
    std::string template_name() const { return e.template_name ? e.template_name : ""; }
    std::string file() const { return e.file ? e.file : ""; }
    std::string message() const { return e.message ? e.message : ""; }
    std::string chain_text() const {
      std::string s;
      for (const auto & link : chain) {
        s += std::string(s.empty() ? "" : " ") + link.template_name + ":" + std::to_string(link.line);
      }
      return s;
    }
  };
  size_t error_count() const { return gltang_execution_error_count(execution); }
  Entry error(size_t index) const {
    Entry entry;
    std::memset(&entry.e, 0, sizeof(entry.e));
    EXPECT_TRUE(gltang_execution_error(execution, index, &entry.e)) << "entry " << index;
    for (size_t i = 0; i < gltang_execution_error_chain_count(execution, index); ++i) {
      GLTANG_ErrorLink link;
      EXPECT_TRUE(gltang_execution_error_chain(execution, index, i, &link));
      entry.chain.push_back(link);
    }
    EXPECT_EQ(entry.chain.size(), entry.e.chain_count);
    return entry;
  }

  /// The raw output: every segment unencoded.
  std::string raw() const {
    size_t length = 0;
    const char * o = gltang_execution_output_raw(execution, &length);
    return std::string(o, length);
  }
  /// The output with every segment encoded per its tag.
  std::string rendered() const {
    char * r = nullptr;
    size_t length = 0;
    if (gltang_execution_output_render(execution, &r, &length) != GLTANG_OK) {
      return "<render failed>";
    }
    std::string s(r, length);
    gltang_buffer_free(r);
    return s;
  }

};

/// A source compiled and run to its end, in one object.
class Run {
 public:
  Compiled compiled;
  Context context;

  explicit Run(const std::string & source, Mode mode = Mode::Script, const Config & config = Config(), bool execute = true)
    : compiled(source, mode), context(compiled.program, config) {
    EXPECT_TRUE(compiled.ok()) << source << ": " << compiled.error.message;
    EXPECT_TRUE(context.ok()) << source;
    if (execute && context.ok()) {
      EXPECT_TRUE(context.execute()) << source;
    }
  }
};

// ---------------------------------------------------------------------------
// One-line expectations
// ---------------------------------------------------------------------------

#define TT_EXPECT_INTEGER(source, expected) \
  do { \
    tt::Run run_(source); \
    ASSERT_TRUE(run_.context.is_integer()) << (source) << " gave " << run_.context.describe(); \
    ASSERT_EQ(run_.context.integer(), (int64_t)(expected)) << (source); \
  } while (0)

#define TT_EXPECT_BOOLEAN(source, expected) \
  do { \
    tt::Run run_(source); \
    ASSERT_TRUE(run_.context.is_bool()) << (source) << " gave " << run_.context.describe(); \
    ASSERT_EQ(run_.context.boolean(), (bool)(expected)) << (source); \
  } while (0)

#define TT_EXPECT_NULL(source) \
  do { \
    tt::Run run_(source); \
    ASSERT_TRUE(run_.context.is_null()) << (source) << " gave " << run_.context.describe(); \
  } while (0)

#define TT_EXPECT_STRING(source, expected) \
  do { \
    tt::Run run_(source); \
    ASSERT_TRUE(run_.context.is_string()) << (source) << " gave " << run_.context.describe(); \
    ASSERT_EQ(run_.context.text(), std::string(expected)) << (source); \
  } while (0)

/// The result is an error, and its text is `Error: ` and the message (or the marker).
#define TT_EXPECT_ERROR(source, expected) \
  do { \
    tt::Run run_(source); \
    ASSERT_TRUE(run_.context.is_error()) << (source) << " gave " << run_.context.describe(); \
    ASSERT_EQ(run_.context.describe(), std::string(expected)) << (source); \
  } while (0)

#define TT_EXPECT_OUTPUT(source, expected) \
  do { \
    tt::Run run_(source); \
    ASSERT_EQ(run_.context.raw(), std::string(expected)) << (source); \
  } while (0)

// The same expectations as functions, the way the ported ctang tests call them.

inline void expect_integer(const char * code, int64_t expected) {
  TT_EXPECT_INTEGER(code, expected);
}

inline void expect_boolean(const char * code, bool expected) {
  TT_EXPECT_BOOLEAN(code, expected);
}

inline void expect_null(const char * code) {
  TT_EXPECT_NULL(code);
}

inline void expect_string(const char * code, const char * expected) {
  TT_EXPECT_STRING(code, expected);
}

inline void expect_error(const char * code, const char * expected) {
  TT_EXPECT_ERROR(code, expected);
}

inline void expect_output(const char * code, const char * expected) {
  TT_EXPECT_OUTPUT(code, expected);
}

}  // namespace tt

#endif
