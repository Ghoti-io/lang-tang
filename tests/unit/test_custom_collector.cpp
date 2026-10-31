// The collector model of host-defined types (type.h, "The collector model" and
// "Copy semantics"): weak slots, the raw bytes in place and the pin that keeps
// them there, finalizers and the teardown order that keeps their registry,
// value semantics, and identity by stable ID.
//
// The types under test (each made by a native of the library `c`):
//   node   a reference type: three slots, 0 and 2 weak, 1 strong; eight bytes.
//   res    a reference type with a finalizer: sixteen bytes, the first eight a tag. Its `negate` (`-x`) is
//          the bytes pointer of its operand, held across allocations (as `opbytes` is for a native's).
//   point  a value type with no `copy`: slots 0 and 1 strong, 2 weak; eight bytes.
//   vcopy  a value type with a `copy` and a finalizer: one slot, eight bytes. What its
//          `copy` answers is set by the test (Probe::copy_mode).
//   plain  declares nothing of this story: two slots, eight bytes.
//   bare   one slot and no raw bytes.
//   bigv   a value type: one slot and 64 KiB of raw bytes, so that its copy needs memory of its own.
// The finalizer records every call: the tag, the pointer it was given, its size, and
// how many registries the process had freed when it ran.
//
// Tests that depend on the collector force torture and verify on inside the test, so
// that a plain `make test` runs them too; they also take the moving stack and, in the
// relocation arm, the relocating heap from the environment (a test that must see an
// object move says so when relocation is on, so that the instrument is seen to run).
// The suite is in TORTURE_BOUNDED.

#include "exec_harness.h"
#include "test_helpers.h"
#include "watchdog.h"

#include <ghoti.io/lang-tang/type.h>
#include <ghoti.io/runtime-heap/id.h>
#include <ghoti.io/runtime-heap/roots.h>

#include <cstdlib>
#include <cstring>
#include <functional>
#include <set>
#include <string>
#include <vector>

using tt::Compiled;
using tt::Config;
using tt::Context;

namespace {

/// What every type's callbacks and the natives share, for one test.
struct Probe {
  GLTANG_TypeRegistry * registry = nullptr;
  const GLTANG_Type * node = nullptr;
  const GLTANG_Type * res = nullptr;
  const GLTANG_Type * point = nullptr;
  const GLTANG_Type * vcopy = nullptr;
  const GLTANG_Type * plain = nullptr;
  const GLTANG_Type * bare = nullptr;
  const GLTANG_Type * bigv = nullptr;
  Context * context = nullptr;
  // The finalizer's record.
  std::vector<int64_t> final_tags;
  std::vector<void *> final_bytes;
  std::vector<size_t> final_sizes;
  std::vector<uint64_t> final_freed;   ///< gltang_vm_test_registries_freed() when each ran.
  // vcopy's `copy`: 0 a new instance (its tag plus 1000), 1 `self`, 2 an instance of another type, 3 an error, 4 a decline.
  int copy_mode = 0;
  int copy_calls = 0;
  std::vector<std::string> failures;   ///< What a callback found wrong (a callback cannot ASSERT).

  Probe();
  ~Probe() { gltang_type_registry_release(registry); }
  Probe(const Probe &) = delete;
  Probe & operator=(const Probe &) = delete;
};

Probe * probe_of(void * user) {
  return static_cast<Probe *>(user);
}

void finalize(void * user, void * bytes, size_t byte_count) {
  Probe * p = probe_of(user);
  int64_t tag = 0;
  std::memcpy(&tag, bytes, sizeof tag);
  p->final_tags.push_back(tag);
  p->final_bytes.push_back(bytes);
  p->final_sizes.push_back(byte_count);
  p->final_freed.push_back(gltang_vm_test_registries_freed());
}

int64_t tag_of(GLTANG_NativeCall * call, GLTANG_Handle h) {
  int64_t tag = 0;
  (void)gltang_handle_custom_read_bytes(call, h, 0, &tag, sizeof tag);
  return tag;
}

GLTANG_OpStatus vcopy_copy(GLTANG_NativeCall * call, void * user, GLTANG_Handle self) {
  Probe * p = probe_of(user);
  ++p->copy_calls;
  GLTANG_Handle h;
  switch (p->copy_mode) {
    case 0: {
      if (gltang_handle_make_custom(call, p->vcopy, &h) != GLTANG_OK) {
        return GLTANG_OP_ERROR;
      }
      int64_t tag = tag_of(call, self) + 1000;
      (void)gltang_handle_custom_write_bytes(call, h, 0, &tag, sizeof tag);
      gltang_call_return_handle(call, h);
      return GLTANG_OP_OK;
    }
    case 1:
      gltang_call_return_handle(call, self);
      return GLTANG_OP_OK;
    case 2:
      if (gltang_handle_make_custom(call, p->node, &h) != GLTANG_OK) {
        return GLTANG_OP_ERROR;
      }
      gltang_call_return_handle(call, h);
      return GLTANG_OP_OK;
    case 3:
      gltang_call_return_error(call, GLTANG_ERROR_DIVIDE_BY_ZERO);
      return GLTANG_OP_ERROR;
    default:
      return GLTANG_OP_DECLINE;
  }
}

const uint32_t node_weak[] = {0, 2};

GLTANG_OpStatus res_negate(GLTANG_NativeCall * call, void * user, GLTANG_Handle self);
const uint32_t point_weak[] = {2};

Probe::Probe() {
  EXPECT_EQ(gltang_type_registry_create(&registry), GLTANG_OK);
  GLTANG_TypeDescriptor d = GLTANG_TYPE_DESCRIPTOR_INIT;
  d.name = "node";
  d.slot_count = 3;
  d.byte_count = 8;
  d.thread_safe = true;
  d.user = this;
  d.weak_slots = node_weak;
  d.weak_slot_count = 2;
  EXPECT_EQ(gltang_type_registry_add(registry, &d, &node), GLTANG_OK);
  GLTANG_TypeDescriptor r = GLTANG_TYPE_DESCRIPTOR_INIT;
  r.name = "res";
  r.byte_count = 16;
  r.thread_safe = true;
  r.user = this;
  r.finalizer = finalize;
  r.negate = res_negate;
  EXPECT_EQ(gltang_type_registry_add(registry, &r, &res), GLTANG_OK);
  GLTANG_TypeDescriptor v = GLTANG_TYPE_DESCRIPTOR_INIT;
  v.name = "point";
  v.slot_count = 3;
  v.byte_count = 8;
  v.thread_safe = true;
  v.user = this;
  v.copy_semantics = GLTANG_COPY_VALUE;
  v.weak_slots = point_weak;
  v.weak_slot_count = 1;
  EXPECT_EQ(gltang_type_registry_add(registry, &v, &point), GLTANG_OK);
  GLTANG_TypeDescriptor c = GLTANG_TYPE_DESCRIPTOR_INIT;
  c.name = "vcopy";
  c.slot_count = 1;
  c.byte_count = 8;
  c.thread_safe = true;
  c.user = this;
  c.copy_semantics = GLTANG_COPY_VALUE;
  c.copy = vcopy_copy;
  c.finalizer = finalize;
  EXPECT_EQ(gltang_type_registry_add(registry, &c, &vcopy), GLTANG_OK);
  GLTANG_TypeDescriptor n = GLTANG_TYPE_DESCRIPTOR_INIT;
  n.name = "plain";
  n.slot_count = 2;
  n.byte_count = 8;
  n.thread_safe = true;
  n.user = this;
  EXPECT_EQ(gltang_type_registry_add(registry, &n, &plain), GLTANG_OK);
  GLTANG_TypeDescriptor b = GLTANG_TYPE_DESCRIPTOR_INIT;
  b.name = "bare";
  b.slot_count = 1;
  b.thread_safe = true;
  b.user = this;
  EXPECT_EQ(gltang_type_registry_add(registry, &b, &bare), GLTANG_OK);
  GLTANG_TypeDescriptor g = GLTANG_TYPE_DESCRIPTOR_INIT;
  g.name = "bigv";
  g.slot_count = 1;
  g.byte_count = 65536;
  g.thread_safe = true;
  g.user = this;
  g.copy_semantics = GLTANG_COPY_VALUE;
  EXPECT_EQ(gltang_type_registry_add(registry, &g, &bigv), GLTANG_OK);
}

// ---------------------------------------------------------------------------
// The natives of `c`
// ---------------------------------------------------------------------------

bool arg(GLTANG_NativeCall * call, size_t i, GLTANG_Handle * out) {
  return gltang_call_arg(call, i, out) == GLTANG_OK;
}

/// Makes an instance of `type` with `tag` (argument 0, when given) in its first eight bytes.
bool make_tagged(GLTANG_NativeCall * call, const GLTANG_Type * type) {
  GLTANG_Handle h, t;
  if (gltang_handle_make_custom(call, type, &h) != GLTANG_OK) {
    return false;
  }
  if (arg(call, 0, &t)) {
    int64_t tag = gltang_handle_integer(call, t);
    (void)gltang_handle_custom_write_bytes(call, h, 0, &tag, sizeof tag);
  }
  gltang_call_return_handle(call, h);
  return true;
}

bool n_node(GLTANG_NativeCall * call, void * u) { return make_tagged(call, probe_of(u)->node); }
bool n_res(GLTANG_NativeCall * call, void * u) { return make_tagged(call, probe_of(u)->res); }
bool n_point(GLTANG_NativeCall * call, void * u) { return make_tagged(call, probe_of(u)->point); }
bool n_vcopy(GLTANG_NativeCall * call, void * u) { return make_tagged(call, probe_of(u)->vcopy); }
bool n_plain(GLTANG_NativeCall * call, void * u) { return make_tagged(call, probe_of(u)->plain); }
bool n_bigv(GLTANG_NativeCall * call, void * u) { return make_tagged(call, probe_of(u)->bigv); }
bool n_bare(GLTANG_NativeCall * call, void * u) {
  GLTANG_Handle h;
  if (gltang_handle_make_custom(call, probe_of(u)->bare, &h) != GLTANG_OK) {
    return false;
  }
  gltang_call_return_handle(call, h);
  return true;
}

/// failmake(): makes a res and fails before writing anything into it (a constructor that failed after `make`).
bool n_failmake(GLTANG_NativeCall * call, void * u) {
  GLTANG_Handle h;
  (void)gltang_handle_make_custom(call, probe_of(u)->res, &h);
  gltang_call_return_error(call, GLTANG_ERROR_HOST_FAILED);
  return false;
}

/// set(o, i, v): the GLTANG_Result of storing v in slot i, as an integer (0 is GLTANG_OK).
bool n_set(GLTANG_NativeCall * call, void *) {
  GLTANG_Handle o, i, v;
  if (!arg(call, 0, &o) || !arg(call, 1, &i) || !arg(call, 2, &v)) {
    return false;
  }
  gltang_call_return_integer(call, static_cast<int64_t>(gltang_handle_custom_slot_set(call, o, static_cast<size_t>(gltang_handle_integer(call, i)), v)));
  return true;
}

/// setinner(o, i, j, v): stores v in slot j of the instance in strong slot i of o, which is not an operand of the call.
bool n_setinner(GLTANG_NativeCall * call, void *) {
  GLTANG_Handle o, i, j, v, inner;
  if (!arg(call, 0, &o) || !arg(call, 1, &i) || !arg(call, 2, &j) || !arg(call, 3, &v) ||
      gltang_handle_custom_slot_get(call, o, static_cast<size_t>(gltang_handle_integer(call, i)), &inner) != GLTANG_OK) {
    return false;
  }
  gltang_call_return_integer(call, static_cast<int64_t>(gltang_handle_custom_slot_set(call, inner, static_cast<size_t>(gltang_handle_integer(call, j)), v)));
  return true;
}

/// get(o, i): what slot i holds (a weak slot's target).
bool n_get(GLTANG_NativeCall * call, void *) {
  GLTANG_Handle o, i, out;
  if (!arg(call, 0, &o) || !arg(call, 1, &i) || gltang_handle_custom_slot_get(call, o, static_cast<size_t>(gltang_handle_integer(call, i)), &out) != GLTANG_OK) {
    return false;
  }
  gltang_call_return_handle(call, out);
  return true;
}

/// cleared(o, i): whether weak slot i had a target the collector cleared.
bool n_cleared(GLTANG_NativeCall * call, void *) {
  GLTANG_Handle o, i;
  bool cleared = true;
  if (!arg(call, 0, &o) || !arg(call, 1, &i) || gltang_handle_custom_slot_cleared(call, o, static_cast<size_t>(gltang_handle_integer(call, i)), &cleared) != GLTANG_OK) {
    return false;
  }
  gltang_call_return_bool(call, cleared);
  return true;
}

/// tag(o): the first eight bytes; settag(o, n) writes them.
bool n_tag(GLTANG_NativeCall * call, void *) {
  GLTANG_Handle o;
  if (!arg(call, 0, &o)) {
    return false;
  }
  gltang_call_return_integer(call, tag_of(call, o));
  return true;
}

bool n_settag(GLTANG_NativeCall * call, void *) {
  GLTANG_Handle o, n;
  if (!arg(call, 0, &o) || !arg(call, 1, &n)) {
    return false;
  }
  int64_t tag = gltang_handle_integer(call, n);
  gltang_call_return_integer(call, static_cast<int64_t>(gltang_handle_custom_write_bytes(call, o, 0, &tag, sizeof tag)));
  return true;
}

/// mark(o): marks o as standing for an error; marked(o) reads the mark.
bool n_mark(GLTANG_NativeCall * call, void *) {
  GLTANG_Handle o;
  if (!arg(call, 0, &o)) {
    return false;
  }
  (void)gltang_handle_custom_set_error(call, o, true);
  gltang_call_return_null(call);
  return true;
}

bool n_marked(GLTANG_NativeCall * call, void *) {
  GLTANG_Handle o;
  if (!arg(call, 0, &o)) {
    return false;
  }
  gltang_call_return_bool(call, gltang_handle_custom_is_error(call, o));
  return true;
}

/// collect(): a full collection now.
bool n_collect(GLTANG_NativeCall * call, void * u) {
  EXPECT_EQ(grheap_collect(probe_of(u)->context->heap), GRHEAP_OK);
  gltang_call_return_null(call);
  return true;
}

/// finals(): how many finalizer calls so far.
bool n_finals(GLTANG_NativeCall * call, void * u) {
  gltang_call_return_integer(call, static_cast<int64_t>(probe_of(u)->final_tags.size()));
  return true;
}

/// id(o): o's stable ID; fromid(n): the instance, or the GLTANG_Result of the refusal as a negative integer.
bool n_id(GLTANG_NativeCall * call, void *) {
  GLTANG_Handle o;
  uint64_t id = 0;
  if (!arg(call, 0, &o)) {
    return false;
  }
  GLTANG_Result r = gltang_handle_custom_id(call, o, &id);
  gltang_call_return_integer(call, r == GLTANG_OK ? static_cast<int64_t>(id) : -static_cast<int64_t>(r));
  return true;
}

bool n_fromid(GLTANG_NativeCall * call, void *) {
  GLTANG_Handle n, out;
  if (!arg(call, 0, &n)) {
    return false;
  }
  GLTANG_Result r = gltang_handle_custom_from_id(call, static_cast<uint64_t>(gltang_handle_integer(call, n)), &out);
  if (r != GLTANG_OK) {
    gltang_call_return_integer(call, -static_cast<int64_t>(r));
    return true;
  }
  gltang_call_return_handle(call, out);
  return true;
}

/// ihash(o): o's identity hash, as a string of its decimal digits (a 64-bit number is not a Tang integer).
bool n_ihash(GLTANG_NativeCall * call, void *) {
  GLTANG_Handle o;
  uint64_t hash = 0;
  if (!arg(call, 0, &o) || gltang_handle_custom_identity_hash(call, o, &hash) != GLTANG_OK) {
    return false;
  }
  std::string s = std::to_string(hash);
  gltang_call_return_string(call, s.data(), s.size(), GLTANG_UNICODE_STRING_TYPE_TRUSTED);
  return true;
}

/// same(a, b): whether a and b are one object.
bool n_same(GLTANG_NativeCall * call, void *) {
  GLTANG_Handle a, b;
  if (!arg(call, 0, &a) || !arg(call, 1, &b)) {
    return false;
  }
  void * x = gltang_vm_test_handle_object(call, a);
  gltang_call_return_bool(call, x != nullptr && x == gltang_vm_test_handle_object(call, b));
  return true;
}

/// cellid(o, i): the stable ID of weak slot i's cell, 0 when it has none; alive(n): whether that object lives.
bool n_cellid(GLTANG_NativeCall * call, void * u) {
  GLTANG_Handle o, i;
  if (!arg(call, 0, &o) || !arg(call, 1, &i)) {
    return false;
  }
  void * cell = gltang_vm_test_weak_cell(call, o, static_cast<size_t>(gltang_handle_integer(call, i)));
  uint64_t id = 0;
  if (cell) {
    EXPECT_EQ(grheap_id_of(probe_of(u)->context->heap, cell, &id), GRHEAP_OK);
  }
  gltang_call_return_integer(call, static_cast<int64_t>(id));
  return true;
}

bool n_alive(GLTANG_NativeCall * call, void * u) {
  GLTANG_Handle n;
  if (!arg(call, 0, &n)) {
    return false;
  }
  void * object = nullptr;
  gltang_call_return_bool(call, grheap_object_of_id(probe_of(u)->context->heap, static_cast<uint64_t>(gltang_handle_integer(call, n)), &object) == GRHEAP_OK);
  return true;
}

/// pins(): the pins outstanding in the heap.
bool n_pins(GLTANG_NativeCall * call, void * u) {
  GRHEAP_Stats stats;
  EXPECT_EQ(grheap_stats(probe_of(u)->context->heap, &stats), GRHEAP_OK);
  gltang_call_return_integer(call, static_cast<int64_t>(stats.pins));
  return true;
}

uint64_t heap_pins(Probe * p) {
  GRHEAP_Stats stats;
  EXPECT_EQ(grheap_stats(p->context->heap, &stats), GRHEAP_OK);
  return stats.pins;
}

/// Allocates and collects: forty arrays, each a GC point, and a full collection.
void churn(GLTANG_NativeCall * call, Probe * p) {
  for (int i = 0; i < 40; ++i) {
    GLTANG_Handle a;
    if (gltang_handle_push_frame(call) != GLTANG_OK) {
      p->failures.push_back("push_frame");
      return;
    }
    (void)gltang_handle_make_array(call, 4, &a);
    (void)gltang_handle_pop_frame(call);
  }
  (void)grheap_collect(p->context->heap);
}

/**
 * Writes through the pointer `bytes` (of `size` bytes) after the churn and reads the instance back through its
 * handle: the two agree only if the pointer still addresses the instance's bytes.
 */
void write_after_churn(GLTANG_NativeCall * call, Probe * p, GLTANG_Handle h, unsigned char * bytes, size_t size, unsigned char fill) {
  churn(call, p);
  std::memset(bytes, fill, size);
  std::vector<unsigned char> back(size);
  if (gltang_handle_custom_read_bytes(call, h, 0, back.data(), size) != GLTANG_OK) {
    p->failures.push_back("read_bytes");
    return;
  }
  for (size_t i = 0; i < size; ++i) {
    if (back[i] != fill) {
      p->failures.push_back("the bytes pointer no longer addresses the instance after the callback allocated");
      return;
    }
  }
}

/// opbytes(o): the bytes pointer of an operand, stable across the callback's allocations; answers the pins it made (0).
bool n_opbytes(GLTANG_NativeCall * call, void * u) {
  Probe * p = probe_of(u);
  GLTANG_Handle o;
  void * bytes = nullptr;
  size_t size = 0;
  if (!arg(call, 0, &o) || gltang_handle_custom_bytes(call, o, &bytes, &size) != GLTANG_OK) {
    return false;
  }
  const uint64_t pins = heap_pins(p);
  write_after_churn(call, p, o, static_cast<unsigned char *>(bytes), size, 0x5a);
  gltang_call_return_integer(call, static_cast<int64_t>(pins));
  return true;
}

GLTANG_OpStatus res_negate(GLTANG_NativeCall * call, void * user, GLTANG_Handle self) {
  Probe * p = probe_of(user);
  void * bytes = nullptr;
  size_t size = 0;
  if (gltang_handle_custom_bytes(call, self, &bytes, &size) != GLTANG_OK) {
    return GLTANG_OP_ERROR;
  }
  const uint64_t pins = heap_pins(p);
  write_after_churn(call, p, self, static_cast<unsigned char *>(bytes), size, 0x3c);
  gltang_call_return_integer(call, static_cast<int64_t>(heap_pins(p) - pins));
  return GLTANG_OP_OK;
}

/**
 * slotbytes(holder, i, twice): the bytes pointer of the instance in strong slot i of `holder` (not an operand),
 * asked for once or twice; answers how many pins the callback holds (the pins before it counted out).
 */
bool n_slotbytes(GLTANG_NativeCall * call, void * u) {
  Probe * p = probe_of(u);
  GLTANG_Handle holder, i, twice, inner;
  if (!arg(call, 0, &holder) || !arg(call, 1, &i) || !arg(call, 2, &twice)) {
    return false;
  }
  const uint64_t before = heap_pins(p);
  if (gltang_handle_custom_slot_get(call, holder, static_cast<size_t>(gltang_handle_integer(call, i)), &inner) != GLTANG_OK) {
    return false;
  }
  void * bytes = nullptr;
  size_t size = 0;
  if (gltang_handle_custom_bytes(call, inner, &bytes, &size) != GLTANG_OK) {
    return false;
  }
  if (gltang_handle_bool(call, twice)) {
    void * again = nullptr;
    size_t size2 = 0;
    if (gltang_handle_custom_bytes(call, inner, &again, &size2) != GLTANG_OK || again != bytes || size2 != size) {
      p->failures.push_back("a second bytes pointer of one instance differs");
    }
  }
  const uint64_t held = heap_pins(p) - before;
  write_after_churn(call, p, inner, static_cast<unsigned char *>(bytes), size, 0xa5);
  gltang_call_return_integer(call, static_cast<int64_t>(held));
  return true;
}

/**
 * bytesptr(o): records the bytes pointer of o in `seen_pointers`, so that a test with a heap that does not move can
 * compare it with the pointer the finalizer is given.
 */
std::vector<void *> * seen_pointers = nullptr;
bool n_bytesptr(GLTANG_NativeCall * call, void *) {
  GLTANG_Handle o;
  void * bytes = nullptr;
  size_t size = 0;
  if (!arg(call, 0, &o) || gltang_handle_custom_bytes(call, o, &bytes, &size) != GLTANG_OK) {
    return false;
  }
  if (seen_pointers) {
    seen_pointers->push_back(bytes);
  }
  gltang_call_return_null(call);
  return true;
}

/// freed(): the registries the process has freed.
bool n_freed(GLTANG_NativeCall * call, void *) {
  gltang_call_return_integer(call, static_cast<int64_t>(gltang_vm_test_registries_freed()));
  return true;
}

struct Out {
  bool finished = false;
  std::string described;
  std::string raw;
  GLTANG_ValueKind kind = GLTANG_KIND_NULL;
  int64_t integer = 0;
  bool boolean = false;
  GLTANG_JitStats jit{};
  uint64_t moved = 0;          ///< Objects the relocating heap moved (0 when it is not relocating).
  uint64_t frees = 0;          ///< Objects the heap freed by the end of the run.
  uint64_t memory_peak = 0;    ///< The context's memory peak.
  GLTANG_ErrorKind error = GLTANG_ERROR_KIND_COUNT;  ///< The result's error kind, when it is an error.
};

using RunSetup = std::function<void(Context &, Probe &)>;

/// Whether the relocation arm asked for a heap that moves every object (the environment's choice, which the tests take).
bool relocation_requested() {
#ifdef GLTANG_TEST_HAS_RELOCATE
  const char * v = std::getenv("GRHEAP_RELOCATE");
  return v && *v && std::strcmp(v, "0") != 0;
#else
  return false;
#endif
}

/**
 * Runs `source` with `use c;` bound to the natives above and the registry attached. With `drop_registry`, the probe's
 * own reference to the registry is let go once it is attached, so that only the execution and the instances hold it.
 * The context is destroyed before this returns, so every finalizer has run by then.
 */
Out run(const std::string & source, Probe & probe, const Config & config = Config(), const RunSetup & setup = RunSetup(), bool drop_registry = false) {
  Out out;
  Compiled compiled(source);
  EXPECT_TRUE(compiled.ok()) << source << ": " << compiled.error.message;
  if (!compiled.ok()) {
    return out;
  }
  {
    Context context(compiled.program, config);
    EXPECT_TRUE(context.ok());
    if (!context.ok()) {
      return out;
    }
    probe.context = &context;
    GLTANG_Library * c = nullptr;
    EXPECT_EQ(gltang_library_create("c", &c), GLTANG_OK);
    struct { const char * name; GLTANG_NativeFn fn; } natives[] = {
      {"node", n_node}, {"res", n_res}, {"point", n_point}, {"vcopy", n_vcopy}, {"plain", n_plain}, {"bare", n_bare}, {"bigv", n_bigv},
      {"failmake", n_failmake}, {"set", n_set}, {"setinner", n_setinner}, {"get", n_get}, {"cleared", n_cleared}, {"tag", n_tag},
      {"settag", n_settag}, {"mark", n_mark}, {"marked", n_marked}, {"collect", n_collect}, {"finals", n_finals},
      {"id", n_id}, {"fromid", n_fromid}, {"ihash", n_ihash}, {"same", n_same}, {"cellid", n_cellid},
      {"alive", n_alive}, {"pins", n_pins}, {"opbytes", n_opbytes}, {"slotbytes", n_slotbytes},
      {"bytesptr", n_bytesptr}, {"freed", n_freed},
    };
    for (const auto & n : natives) {
      EXPECT_EQ(gltang_library_add_native(c, n.name, n.fn, &probe), GLTANG_OK);
    }
    EXPECT_EQ(gltang_library_add_library(context.library(), c), GLTANG_OK);
    gltang_library_release(c);
    EXPECT_EQ(gltang_execution_attach_types(context.execution, probe.registry), GLTANG_OK);
    if (drop_registry) {
      gltang_type_registry_release(probe.registry);
      probe.registry = nullptr;
    }
    if (setup) {
      setup(context, probe);
    }
    out.finished = context.execute();
    out.raw = context.raw();
    out.described = context.describe();
    out.kind = context.kind();
    if (context.is_integer()) {
      out.integer = context.integer();
    }
    if (context.is_bool()) {
      out.boolean = context.boolean();
    }
    out.jit = context.jit_stats();
    out.memory_peak = grcore_context_memory_peak(context.context);
    if (context.is_error()) {
      out.error = context.error_kind();
    }
    GRHEAP_Stats stats;
    EXPECT_EQ(grheap_stats(context.heap, &stats), GRHEAP_OK);
    out.frees = stats.frees;
#ifdef GLTANG_TEST_HAS_RELOCATE
    GRHEAP_RelocationStats moved;
    std::memset(&moved, 0, sizeof moved);
    if (grheap_relocation_stats(context.heap, &moved) == GRHEAP_OK) {
      out.moved = moved.moved;
    }
#endif
    probe.context = nullptr;
  }
  for (const std::string & f : probe.failures) {
    ADD_FAILURE() << source << ": " << f;
  }
  probe.failures.clear();
  return out;
}

Config forced_torture() {
  Config c;
  c.torture = 1;
  c.verify = 1;
  return c;
}

/// The instruments the collector tests run under, each in turn: plain, torture with verify, and the moving stack.
std::vector<Config> collector_arms() {
  std::vector<Config> arms(3);
  arms[1] = forced_torture();
  arms[2].moving_stack = 1;
  return arms;
}

#define USE "use c; "

}  // namespace

// ---------------------------------------------------------------------------
// The descriptor: registration, the old size, the table hash
// ---------------------------------------------------------------------------

namespace {

/// The table hash as type.h states it for a descriptor that declares none of the collector's members, computed here
/// without the engine: FNV-1a over the name, its length, the version, the counts, `thread_safe` and the operations.
uint64_t reference_hash(const char * name, uint32_t version, uint32_t slots, uint32_t bytes, bool thread_safe, uint32_t operations) {
  uint64_t h = UINT64_C(0xcbf29ce484222325);
  auto fold = [&h](const void * p, size_t n) {
    const unsigned char * b = static_cast<const unsigned char *>(p);
    for (size_t i = 0; i < n; ++i) {
      h ^= b[i];
      h *= UINT64_C(0x100000001b3);
    }
  };
  auto fold64 = [&fold](uint64_t v) {
    unsigned char b[8];
    for (size_t i = 0; i < 8; ++i) {
      b[i] = static_cast<unsigned char>(v >> (8u * i));
    }
    fold(b, 8);
  };
  size_t length = std::strlen(name);
  fold(name, length);
  fold64(length);
  fold64(version);
  fold64(slots);
  fold64(bytes);
  fold64(thread_safe ? 1u : 0u);
  fold64(operations);
  return h;
}

uint64_t hash_of(const std::function<void(GLTANG_TypeDescriptor &)> & change, GLTANG_Result * result = nullptr) {
  GLTANG_TypeRegistry * x = nullptr;
  EXPECT_EQ(gltang_type_registry_create(&x), GLTANG_OK);
  GLTANG_TypeDescriptor f = GLTANG_TYPE_DESCRIPTOR_INIT;
  f.name = "h";
  f.slot_count = 4;
  f.byte_count = 8;
  change(f);
  const GLTANG_Type * t = nullptr;
  GLTANG_Result r = gltang_type_registry_add(x, &f, &t);
  if (result) {
    *result = r;
  }
  uint64_t h = r == GLTANG_OK ? gltang_type_table_hash(t) : 0;
  gltang_type_registry_release(x);
  return h;
}

void finalize_nothing(void *, void *, size_t) {
}

GLTANG_OpStatus copy_nothing(GLTANG_NativeCall *, void *, GLTANG_Handle) {
  return GLTANG_OP_DECLINE;
}

}  // namespace

TEST(CollectorRegistry, ADescriptorThatDeclaresNothingNewHashesAsBeforeAndEachNewThingEntersTheHash) {
  const uint64_t h0 = hash_of([](GLTANG_TypeDescriptor &) {});
  EXPECT_EQ(h0, reference_hash("h", 0, 4, 8, false, 0));
  // Sized for the descriptor of the version before (story 9's last member): the same type.
  EXPECT_EQ(hash_of([](GLTANG_TypeDescriptor & f) { f.size = offsetof(GLTANG_TypeDescriptor, copy_semantics); }), h0);
  static const uint32_t w1[] = {1};
  static const uint32_t w2[] = {2};
  static const uint32_t w12[] = {1, 2};
  static const uint32_t w21[] = {2, 1};
  const uint64_t value = hash_of([](GLTANG_TypeDescriptor & f) { f.copy_semantics = GLTANG_COPY_VALUE; });
  const uint64_t fin = hash_of([](GLTANG_TypeDescriptor & f) { f.finalizer = finalize_nothing; });
  const uint64_t weak1 = hash_of([](GLTANG_TypeDescriptor & f) { f.weak_slots = w1; f.weak_slot_count = 1; });
  const uint64_t weak2 = hash_of([](GLTANG_TypeDescriptor & f) { f.weak_slots = w2; f.weak_slot_count = 1; });
  const uint64_t weak12 = hash_of([](GLTANG_TypeDescriptor & f) { f.weak_slots = w12; f.weak_slot_count = 2; });
  const uint64_t weak21 = hash_of([](GLTANG_TypeDescriptor & f) { f.weak_slots = w21; f.weak_slot_count = 2; });
  const uint64_t copying = hash_of([](GLTANG_TypeDescriptor & f) { f.copy_semantics = GLTANG_COPY_VALUE; f.copy = copy_nothing; });
  std::set<uint64_t> distinct = {h0, value, fin, weak1, weak2, weak12, copying};
  EXPECT_EQ(distinct.size(), 7u) << "each declared thing makes another type";
  EXPECT_EQ(weak12, weak21) << "the weak slots are a set: their order is not a declaration";
  // Never an address: another finalizer, another copy function, the same hash.
  EXPECT_EQ(hash_of([](GLTANG_TypeDescriptor & f) { f.finalizer = finalize; }), fin);
  // An empty list with a pointer is no weak slot at all.
  EXPECT_EQ(hash_of([](GLTANG_TypeDescriptor & f) { f.weak_slots = w1; f.weak_slot_count = 0; }), h0);
}

TEST(CollectorRegistry, RegistrationRefusesWhatTheModelCannotHold) {
  static const uint32_t past[] = {4};
  static const uint32_t twice[] = {1, 1};
  static const uint32_t five[] = {0, 1, 2, 3, 0};
  GLTANG_Result r = GLTANG_OK;
  auto refused = [&r](const std::function<void(GLTANG_TypeDescriptor &)> & change) {
    (void)hash_of(change, &r);
    return r == GLTANG_ERR_INVALID;
  };
  EXPECT_TRUE(refused([](GLTANG_TypeDescriptor & f) { f.weak_slots = past; f.weak_slot_count = 1; })) << "an index past the slots";
  EXPECT_TRUE(refused([](GLTANG_TypeDescriptor & f) { f.weak_slots = twice; f.weak_slot_count = 2; })) << "an index twice";
  EXPECT_TRUE(refused([](GLTANG_TypeDescriptor & f) { f.weak_slots = five; f.weak_slot_count = 5; })) << "more indices than slots";
  EXPECT_TRUE(refused([](GLTANG_TypeDescriptor & f) { f.weak_slots = nullptr; f.weak_slot_count = 1; })) << "a count with no list";
  EXPECT_TRUE(refused([](GLTANG_TypeDescriptor & f) { f.copy_semantics = static_cast<GLTANG_CopySemantics>(2); })) << "a semantics that is neither";
  EXPECT_TRUE(refused([](GLTANG_TypeDescriptor & f) { f.copy = copy_nothing; })) << "a copy on a reference type";
  EXPECT_TRUE(refused([](GLTANG_TypeDescriptor & f) { f.copy_semantics = GLTANG_COPY_VALUE; f.finalizer = finalize_nothing; })) << "a value type with a finalizer and no copy";
  EXPECT_TRUE(refused([](GLTANG_TypeDescriptor & f) {
    static const GLTANG_Continuation table[] = {{"k", nullptr}};
    f.copy_semantics = GLTANG_COPY_VALUE;
    f.copy = copy_nothing;
    f.resumable_ops = 1u << GLTANG_TYPE_OP_COPY;
    f.continuations = table;
    f.continuation_count = 1;
  })) << "a resumable copy";
  // Accepted: the same with the missing part, and a weak slot of every index.
  EXPECT_FALSE(refused([](GLTANG_TypeDescriptor & f) { f.copy_semantics = GLTANG_COPY_VALUE; f.finalizer = finalize_nothing; f.copy = copy_nothing; }));
  static const uint32_t all[] = {3, 0, 2, 1};
  EXPECT_FALSE(refused([](GLTANG_TypeDescriptor & f) { f.weak_slots = all; f.weak_slot_count = 4; }));
}

// ---------------------------------------------------------------------------
// Weak slots
// ---------------------------------------------------------------------------

TEST(WeakSlots, ALiveTargetIsReadAndADeadOneIsClearedWithItsFinalizerRunInEveryArm) {
  for (const Config & arm : collector_arms()) {
    Probe probe;
    // X is held by the variable while the first collection runs, and by nothing after.
    Out o = run(USE "n = c.node(); x = c.res(7); c.set(n, 0, x); c.collect(); "
                "a = [c.same(c.get(n, 0), x), c.cleared(n, 0), c.finals()]; "
                "x = null; c.collect(); "
                "a + [c.get(n, 0) == null, c.cleared(n, 0), c.finals()];",
                probe, arm);
    ASSERT_TRUE(o.finished) << o.described;
    EXPECT_EQ(o.described, "[true, false, 0, true, true, 1]");
    ASSERT_EQ(probe.final_tags.size(), 1u);
    EXPECT_EQ(probe.final_tags[0], 7);
  }
}

TEST(WeakSlots, NeverSetEmptiedAndImmediateSlotsAreNotCleared) {
  for (const Config & arm : collector_arms()) {
    Probe probe;
    Out o = run(USE "function f() { return 1; } n = c.node(); "
                "a = [c.get(n, 0), c.cleared(n, 0)]; "
                "c.set(n, 0, c.res(1)); c.set(n, 0, null); c.collect(); "
                "a = a + [c.get(n, 0), c.cleared(n, 0)]; "
                "c.set(n, 2, true); c.set(n, 0, f); c.collect(); c.collect(); "
                "g = c.get(n, 0); a + [c.get(n, 2), c.cleared(n, 2), g(), c.cleared(n, 0)];",
                probe, arm);
    ASSERT_TRUE(o.finished) << o.described;
    EXPECT_EQ(o.described, "[null, false, null, false, true, false, 1, false]");
  }
}

TEST(WeakSlots, EveryKindOfValueIsAcceptedOrRefusedAndARefusalChangesNothing) {
  // Each value is offered to weak slot 0, which holds the string "keep" first: an accepted value replaces it and
  // reads back as itself (`check`); a refused one is GLTANG_ERR_INVALID and the slot still holds "keep". A strong
  // slot (1) takes every one of them.
  struct Row {
    const char * expr;
    const char * check;   ///< NULL: refused.
  };
  const Row rows[] = {
    {"null", "got == null"}, {"true", "got == true"}, {"false", "got == false"}, {"f", "got() == 1"},
    {"\"s\"", "c.same(got, v)"}, {"c", "c.same(got, v)"}, {"c.node", "c.same(got, v)"}, {"r", "c.same(got, v)"},
    {"c.node()", "c.same(got, v)"}, {"marked", "c.same(got, v)"},
    {"1", nullptr}, {"2305843009213693952", nullptr}, {"1.5", nullptr}, {"[1]", nullptr}, {"{k: 1}", nullptr},
    {"1 / 0", nullptr}, {"c.point()", nullptr}, {"c.vcopy()", nullptr},
  };
  const std::string invalid = std::to_string(static_cast<int>(GLTANG_ERR_INVALID));
  for (const Row & row : rows) {
    Probe probe;
    std::string source = std::string(USE) + "use random; function f() { return 1; } r = random.seeded(1); marked = c.node(); c.mark(marked); "
                         "n = c.node(); keep = \"keep\"; c.set(n, 0, keep); v = " + row.expr + "; "
                         "s = c.set(n, 0, v); got = c.get(n, 0); "
                         "[s, c.set(n, 1, v), " + (row.check ? row.check : "got == keep") + "];";
    Out o = run(source, probe);
    ASSERT_TRUE(o.finished) << source << " gave " << o.described;
    EXPECT_EQ(o.described, (row.check ? "[0, 0, true]" : "[" + invalid + ", 0, true]")) << row.expr;
  }
}

TEST(WeakSlots, ATargetHeldStronglyBesideTheWeakSlotLives) {
  for (const Config & arm : collector_arms()) {
    Probe probe;
    Out o = run(USE "n = c.node(); x = c.res(3); c.set(n, 0, x); c.set(n, 1, x); x = null; c.collect(); "
                "[c.tag(c.get(n, 0)), c.same(c.get(n, 0), c.get(n, 1)), c.cleared(n, 0), c.finals()];",
                probe, arm);
    ASSERT_TRUE(o.finished) << o.described;
    EXPECT_EQ(o.described, "[3, true, false, 0]");
  }
}

TEST(WeakSlots, ACycleThroughAWeakSlotIsCollected) {
  for (const Config & arm : collector_arms()) {
    Probe probe;
    // A's weak slot names B, B's strong slot names A: once the variables let go, both die, and so does A's cell.
    Out o = run(USE "a = c.node(); b = c.node(); c.set(a, 0, b); c.set(b, 1, a); "
                "ids = [c.id(a), c.id(b), c.cellid(a, 0)]; c.collect(); "
                "live = [c.alive(ids[0]), c.alive(ids[1]), c.alive(ids[2])]; "
                "a = null; b = null; c.collect(); "
                "live + [c.alive(ids[0]), c.alive(ids[1]), c.alive(ids[2]), ids[2] > 0];",
                probe, arm);
    ASSERT_TRUE(o.finished) << o.described;
    EXPECT_EQ(o.described, "[true, true, true, false, false, false, true]");
  }
}

TEST(WeakSlots, TheInstanceKeepsItsCellAliveAndTheCellNeverKeepsItsTarget) {
  for (const Config & arm : collector_arms()) {
    Probe probe;
    Out o = run(USE "n = c.node(); x = c.node(); c.set(n, 2, x); cell = c.cellid(n, 2); xid = c.id(x); c.collect(); "
                "a = [c.alive(cell), c.alive(xid)]; x = null; c.collect(); "
                "a + [c.alive(cell), c.alive(xid), c.get(n, 2) == null, c.cleared(n, 2)];",
                probe, arm);
    ASSERT_TRUE(o.finished) << o.described;
    EXPECT_EQ(o.described, "[true, true, true, false, true, true]");
  }
}

TEST(WeakSlots, AWeakTargetThatMovesIsReadAtItsNewAddress) {
  // Under the relocation arm every collection moves every unpinned object: the target, the cell, the instance.
  // The first store into a weak slot makes the cell (a GC point) with the instance read again after it; the
  // instance is reached through another's slot, so it is not an operand of the call, which would pin it.
  Probe probe;
  Config config = forced_torture();
  Out o = run(USE "o = c.node(); c.set(o, 1, c.node()); x = c.res(11); y = c.node(); c.settag(y, 12); "
              "s = [c.setinner(o, 1, 0, x), c.setinner(o, 1, 2, y)]; i = 0; while (i < 20) { [i]; i = i + 1; } c.collect(); "
              "n = c.get(o, 1); "
              "s + [c.tag(c.get(n, 0)), c.same(c.get(n, 0), x), c.tag(c.get(n, 2)), c.same(c.get(n, 2), y)];",
              probe, config);
  ASSERT_TRUE(o.finished) << o.described;
  EXPECT_EQ(o.described, "[0, 0, 11, true, 12, true]");
  if (relocation_requested()) {
    EXPECT_GT(o.moved, 0u) << "the relocating heap moved nothing: the test saw no move";
  }
}

TEST(WeakSlots, ASnapshotIsRefusedWhileACellExistsAndNamesTheType) {
  // The run pauses with a node whose weak slot has a cell: every take is refused, and the refusal names node. The
  // instance names itself through its own hook; a cell left behind by an instance that died (not yet collected)
  // names the type of the instance that made a cell first.
  // The second program lets its instance go before it pauses, and makes no garbage that would start a collection,
  // so the cell is still in the heap, unreachable. Torture would collect it at the first poll: it is off here.
  for (const char * source : {USE "n = c.node(); c.set(n, 0, c.node()); i = 0; while (true) { i = i + 1; }",
                              USE "n = c.node(); c.set(n, 0, c.node()); n = null; i = 0; while (true) { i = i + 1; }"}) {
    Probe probe;
    Config config;
    config.fuel = 2000;
    config.torture = 0;
    Compiled compiled(source);
    ASSERT_TRUE(compiled.ok());
    Context context(compiled.program, config);
    ASSERT_TRUE(context.ok());
    probe.context = &context;
    GLTANG_Library * c = nullptr;
    ASSERT_EQ(gltang_library_create("c", &c), GLTANG_OK);
    ASSERT_EQ(gltang_library_add_native(c, "node", n_node, &probe), GLTANG_OK);
    ASSERT_EQ(gltang_library_add_native(c, "set", n_set, &probe), GLTANG_OK);
    ASSERT_EQ(gltang_library_add_library(context.library(), c), GLTANG_OK);
    gltang_library_release(c);
    ASSERT_EQ(gltang_execution_attach_types(context.execution, probe.registry), GLTANG_OK);
    context.execute();
    ASSERT_TRUE(context.paused()) << source << ": " << context.describe();
    GLTANG_Snapshot * snapshot = nullptr;
    EXPECT_EQ(gltang_snapshot_take(context.execution, &snapshot), GLTANG_ERR_UNSUPPORTED) << source;
    EXPECT_TRUE(gltang_type_same(gltang_snapshot_refused_type(context.execution), probe.node)) << source;
    EXPECT_EQ(snapshot, nullptr);
    probe.context = nullptr;
  }
}

// ---------------------------------------------------------------------------
// The raw bytes in place, and the pin
// ---------------------------------------------------------------------------

TEST(BytesPointer, AnOperandsPointerIsStableAcrossTheCallbacksAllocationsAndCostsNoPin) {
  for (const Config & arm : collector_arms()) {
    Probe probe;
    // opbytes (a native) and `-x` (an operation of the type) check inside that what they wrote through the
    // pointer after allocating is what the handle reads; each answers the pins it made.
    Out o = run(USE "p0 = c.pins(); x = c.res(1); held = c.opbytes(x); [held - p0, -x, c.pins() - p0];",
                probe, arm);
    ASSERT_TRUE(o.finished) << o.described;
    EXPECT_EQ(o.described, "[0, 0, 0]");
  }
}

TEST(BytesPointer, AnotherInstancesPointerIsPinnedOncePerCallbackAndLetGoAfter) {
  for (const Config & arm : collector_arms()) {
    Probe probe;
    // slotbytes checks inside that what it wrote through the pointer after allocating is what the handle reads.
    Out o = run(USE "p0 = c.pins(); n = c.node(); c.set(n, 1, c.res(2)); "
                "once = c.slotbytes(n, 1, false); twice = c.slotbytes(n, 1, true); "
                "[once, twice, c.pins() - p0];",
                probe, arm);
    ASSERT_TRUE(o.finished) << o.described;
    EXPECT_EQ(o.described, "[1, 1, 0]");
  }
}

TEST(BytesPointer, ATypeWithNoBytesAnswersSizeZeroAndPinsNothingAndABadArgumentIsRefused) {
  Probe probe;
  Out o = run(USE "use nobytes; n = c.node(); c.set(n, 1, c.bare()); nobytes(n);", probe, Config(), [&](Context & context, Probe & p) {
    ASSERT_EQ(gltang_library_add_native(context.library(), "nobytes", [](GLTANG_NativeCall * call, void * user) {
      Probe * pr = probe_of(user);
      GLTANG_Handle n, inner, number;
      EXPECT_EQ(gltang_call_arg(call, 0, &n), GLTANG_OK);
      EXPECT_EQ(gltang_handle_custom_slot_get(call, n, 1, &inner), GLTANG_OK);
      const uint64_t before = heap_pins(pr);
      void * bytes = nullptr;
      size_t size = 1;
      EXPECT_EQ(gltang_handle_custom_bytes(call, inner, &bytes, &size), GLTANG_OK);
      EXPECT_EQ(size, 0u);
      EXPECT_NE(bytes, nullptr);
      EXPECT_EQ(heap_pins(pr), before) << "no bytes, so nothing to keep in place";
      EXPECT_EQ(gltang_handle_custom_bytes(call, inner, nullptr, &size), GLTANG_ERR_INVALID);
      EXPECT_EQ(gltang_handle_custom_bytes(call, inner, &bytes, nullptr), GLTANG_ERR_INVALID);
      EXPECT_EQ(gltang_handle_make_integer(call, 3, &number), GLTANG_OK);
      EXPECT_EQ(gltang_handle_custom_bytes(call, number, &bytes, &size), GLTANG_ERR_INVALID);
      bool cleared = true;
      EXPECT_EQ(gltang_handle_custom_slot_cleared(call, number, 0, &cleared), GLTANG_ERR_INVALID);
      EXPECT_EQ(gltang_handle_custom_slot_cleared(call, n, 3, &cleared), GLTANG_ERR_INVALID);
      EXPECT_EQ(gltang_handle_custom_slot_cleared(call, n, 0, nullptr), GLTANG_ERR_INVALID);
      EXPECT_EQ(gltang_handle_custom_slot_cleared(call, n, 1, &cleared), GLTANG_OK);
      EXPECT_FALSE(cleared) << "a strong slot is never cleared";
      uint64_t id = 0;
      EXPECT_EQ(gltang_handle_custom_id(call, n, nullptr), GLTANG_ERR_INVALID);
      EXPECT_EQ(gltang_handle_custom_identity_hash(call, number, &id), GLTANG_ERR_INVALID);
      EXPECT_EQ(gltang_handle_custom_from_id(call, 1, nullptr), GLTANG_ERR_INVALID);
      EXPECT_EQ(gltang_handle_custom_from_id(nullptr, 1, &inner), GLTANG_ERR_INVALID);
      gltang_call_return_integer(call, 1);
      return true;
    }, &p), GLTANG_OK);
  });
  ASSERT_TRUE(o.finished) << o.described;
  EXPECT_EQ(o.integer, 1);
}

TEST(BytesPointer, APinTableThatIsFullIsALimitAndChangesNothing) {
  Probe probe;
  Out o = run(USE "use full; n = c.node(); c.set(n, 1, c.res(4)); full(n);", probe, Config(), [&](Context & context, Probe & p) {
    ASSERT_EQ(gltang_library_add_native(context.library(), "full", [](GLTANG_NativeCall * call, void * user) {
      Probe * pr = probe_of(user);
      GLTANG_Handle n, inner;
      EXPECT_EQ(gltang_call_arg(call, 0, &n), GLTANG_OK);
      EXPECT_EQ(gltang_handle_custom_slot_get(call, n, 1, &inner), GLTANG_OK);
      void * object = gltang_vm_test_handle_object(call, inner);
      // The host fills the object's pin count itself; the engine's pin is then refused.
      for (unsigned i = 0; i < GRHEAP_PIN_MAX; ++i) {
        EXPECT_EQ(grheap_pin(pr->context->heap, object), GRHEAP_OK);
      }
      const uint64_t before = heap_pins(pr);
      void * bytes = nullptr;
      size_t size = 0;
      EXPECT_EQ(gltang_handle_custom_bytes(call, inner, &bytes, &size), GLTANG_ERR_LIMIT);
      EXPECT_EQ(heap_pins(pr), before);
      for (unsigned i = 0; i < GRHEAP_PIN_MAX; ++i) {
        EXPECT_EQ(grheap_unpin(pr->context->heap, object), GRHEAP_OK);
      }
      // And as an operand it needs no pin at all.
      EXPECT_EQ(gltang_handle_custom_bytes(call, n, &bytes, &size), GLTANG_OK);
      gltang_call_return_integer(call, 1);
      return true;
    }, &p), GLTANG_OK);
  });
  ASSERT_TRUE(o.finished) << o.described;
  EXPECT_EQ(o.integer, 1);
}

// ---------------------------------------------------------------------------
// Finalizers
// ---------------------------------------------------------------------------

TEST(Finalizers, EveryUnreachableInstanceIsFinalizedOnceWithItsBytesInEveryArm) {
  for (const Config & arm : collector_arms()) {
    Probe probe;
    Out o = run(USE "i = 0; while (i < 50) { c.res(i + 1); i = i + 1; } c.collect(); c.finals();", probe, arm);
    ASSERT_TRUE(o.finished) << o.described;
    EXPECT_EQ(o.integer, 50);
    std::multiset<int64_t> tags(probe.final_tags.begin(), probe.final_tags.end());
    ASSERT_EQ(tags.size(), 50u);
    for (int64_t t = 1; t <= 50; ++t) {
      EXPECT_EQ(tags.count(t), 1u) << "tag " << t;
    }
    for (size_t s : probe.final_sizes) {
      EXPECT_EQ(s, 16u);
    }
  }
}

TEST(Finalizers, LiveInstancesAreFinalizedOnceWhenTheContextGoesEvenAfterTheHostLetItsRegistryGo) {
  for (int drop = 0; drop < 2; ++drop) {
    Probe probe;
    const uint64_t freed_before = gltang_vm_test_registries_freed();
    Out o = run(USE "keep = []; i = 0; while (i < 20) { keep = keep + [c.res(100 + i)]; i = i + 1; } keep.size;",
                probe, forced_torture(), RunSetup(), drop != 0);
    ASSERT_TRUE(o.finished) << o.described;
    std::multiset<int64_t> tags(probe.final_tags.begin(), probe.final_tags.end());
    ASSERT_EQ(tags.size(), 20u) << "drop " << drop;
    for (int64_t t = 100; t < 120; ++t) {
      EXPECT_EQ(tags.count(t), 1u) << "tag " << t;
    }
    // The registry, its type and `user` (this probe) were there for every one: none was freed before the last ran.
    for (uint64_t f : probe.final_freed) {
      EXPECT_EQ(f, freed_before) << "a registry was freed before a finalizer that needed it ran (drop " << drop << ")";
    }
    // Dropped by the host, the registry went with its last instance; kept, it is still the probe's.
    EXPECT_EQ(gltang_vm_test_registries_freed(), freed_before + (drop ? 1u : 0u));
  }
}

TEST(Finalizers, AnInstanceMadeAndNeverWrittenIsFinalizedOnAllZeros) {
  Probe probe;
  Out o = run(USE "c.failmake(); c.collect(); c.finals();", probe, forced_torture());
  ASSERT_TRUE(o.finished) << o.described;
  EXPECT_EQ(o.integer, 1);
  ASSERT_EQ(probe.final_tags.size(), 1u);
  EXPECT_EQ(probe.final_tags[0], 0);
}

TEST(Finalizers, TheFinalizerIsGivenTheInstancesOwnBytesInPlace) {
  // On a heap that does not move, the pointer a callback reached the bytes by is the one the finalizer gets.
  Probe probe;
  std::vector<void *> seen;
  seen_pointers = &seen;
  Config config;
  config.relocate = 0;
  Out o = run(USE "x = c.res(5); c.bytesptr(x); x = null; c.collect(); c.finals();", probe, config);
  seen_pointers = nullptr;
  ASSERT_TRUE(o.finished) << o.described;
  ASSERT_EQ(seen.size(), 1u);
  ASSERT_EQ(probe.final_bytes.size(), 1u);
  EXPECT_EQ(probe.final_bytes[0], seen[0]);
}

TEST(Finalizers, TortureAndRelocationFinalizeExactlyTheInstancesMade) {
  Probe probe;
  Out o = run(USE "keep = []; i = 0; while (i < 30) { x = c.res(i); if (i % 3 == 0) { keep = keep + [x]; } i = i + 1; } "
              "x = null; c.collect(); [c.finals(), keep.size];",
              probe, forced_torture());
  ASSERT_TRUE(o.finished) << o.described;
  EXPECT_EQ(o.described, "[20, 10]");
  EXPECT_EQ(probe.final_tags.size(), 30u) << "every instance, once: twenty during the run and ten at the end";
  std::multiset<int64_t> tags(probe.final_tags.begin(), probe.final_tags.end());
  for (int64_t t = 0; t < 30; ++t) {
    EXPECT_EQ(tags.count(t), 1u) << "tag " << t;
  }
  if (relocation_requested()) {
    EXPECT_GT(o.moved, 0u);
  }
}

TEST(Finalizers, ATypeThatDeclaresNothingNewAllocatesAndSweepsAsBefore) {
  // A plain instance is one heap object, has no destroy hook (its registry's references do not move when it dies),
  // and is swept like any other. (The context allocator's calls are not counted: the heap's own bookkeeping for a
  // new span is one, and whether one is needed depends on what the heap holds, not on this story.)
  Probe probe;
  const uint64_t freed_before = gltang_vm_test_registries_freed();
  const uint64_t destroys_before = gltang_vm_test_custom_destroys();
  int64_t allocations = -1, frees = -1;
  Out o = run(USE "use plains; plains(200);", probe, Config(), [&](Context & context, Probe & p) {
    struct State { Probe * p; int64_t * allocations; int64_t * frees; };
    static State state;
    state = {&p, &allocations, &frees};
    ASSERT_EQ(gltang_library_add_native(context.library(), "plains", [](GLTANG_NativeCall * call, void *) {
      GLTANG_Handle n;
      EXPECT_EQ(gltang_call_arg(call, 0, &n), GLTANG_OK);
      const int64_t count = gltang_handle_integer(call, n);
      // Once first, collected, so that the measured run reuses the heap's spans, the handle frames and the
      // temporaries the first grew: what is left to count is the instances' own cost.
      for (int64_t i = 0; i < count; ++i) {
        EXPECT_EQ(gltang_handle_push_frame(call), GLTANG_OK);
        GLTANG_Handle warm;
        EXPECT_EQ(gltang_handle_make_custom(call, state.p->plain, &warm), GLTANG_OK);
        EXPECT_EQ(gltang_handle_pop_frame(call), GLTANG_OK);
      }
      EXPECT_EQ(grheap_collect(state.p->context->heap), GRHEAP_OK);
      GRHEAP_Stats before, after;
      EXPECT_EQ(grheap_stats(state.p->context->heap, &before), GRHEAP_OK);
      for (int64_t i = 0; i < count; ++i) {
        EXPECT_EQ(gltang_handle_push_frame(call), GLTANG_OK);
        GLTANG_Handle h;
        EXPECT_EQ(gltang_handle_make_custom(call, state.p->plain, &h), GLTANG_OK);
        EXPECT_EQ(gltang_handle_pop_frame(call), GLTANG_OK);
      }
      EXPECT_EQ(grheap_stats(state.p->context->heap, &after), GRHEAP_OK);
      *state.allocations = static_cast<int64_t>(after.allocations - before.allocations);
      EXPECT_EQ(grheap_collect(state.p->context->heap), GRHEAP_OK);
      GRHEAP_Stats swept;
      EXPECT_EQ(grheap_stats(state.p->context->heap, &swept), GRHEAP_OK);
      // Counted from before the loop: under torture the instances die while it runs.
      *state.frees = static_cast<int64_t>(swept.frees - before.frees);
      gltang_call_return_integer(call, count);
      return true;
    }, &p), GLTANG_OK);
  });
  ASSERT_TRUE(o.finished) << o.described;
  EXPECT_EQ(allocations, 200);
  EXPECT_GE(frees, 200);
  EXPECT_TRUE(probe.final_tags.empty());
  EXPECT_EQ(gltang_vm_test_registries_freed(), freed_before) << "a plain instance's death let a registry reference go";
  EXPECT_EQ(gltang_vm_test_custom_destroys(), destroys_before) << "a plain instance has a destroy hook";
}

// ---------------------------------------------------------------------------
// Copy semantics
// ---------------------------------------------------------------------------

TEST(ValueTypes, AStoredValueTypeIsACopyAndAReferenceTypeIsTheSameObjectInEveryArm) {
  for (const Config & arm : collector_arms()) {
    Probe probe;
    Out o = run(USE "m = c.point(5); a = [m]; b = [0]; b[0] = m; d = {k: m}; e = {k: 0}; e.k = m; "
                "c.settag(m, 6); "
                "r = c.node(); c.settag(r, 1); ra = [r]; "
                "[c.tag(a[0]), c.tag(b[0]), c.tag(d.k), c.tag(e.k), c.same(a[0], m), a[0] == m, c.same(ra[0], r), c.tag(m)];",
                probe, arm);
    ASSERT_TRUE(o.finished) << o.described;
    EXPECT_EQ(o.described, "[5, 5, 5, 5, false, false, true, 6]");
  }
}

TEST(ValueTypes, AnInstanceInsideACopiedContainerIsCopiedToo) {
  Probe probe;
  Out o = run(USE "m = c.point(1); a = [m]; b = [a]; c.settag(a[0], 2); [c.tag(m), c.tag(a[0]), c.tag(b[0][0]), c.same(a[0], b[0][0])];", probe);
  ASSERT_TRUE(o.finished) << o.described;
  EXPECT_EQ(o.described, "[1, 2, 1, false]");
}

TEST(ValueTypes, TheHandleAPIStoresACopy) {
  Probe probe;
  Out o = run(USE "m = c.point(3); n = c.node(); c.set(n, 1, m); c.settag(m, 4); [c.tag(c.get(n, 1)), c.same(c.get(n, 1), m)];", probe);
  ASSERT_TRUE(o.finished) << o.described;
  EXPECT_EQ(o.described, "[3, false]");
}

TEST(ValueTypes, TheDefaultCopyCopiesSlotsBytesAndTheMarkAndGivesTheWeakSlotACellOfItsOwn) {
  for (const Config & arm : collector_arms()) {
    Probe probe;
    // m: slot 0 an array (copied deeply), slot 1 a node (by reference), slot 2 weak to a res held by `x`.
    Out o = run(USE "m = c.point(30566); arr = [1, [2]]; c.set(m, 0, arr); nd = c.node(); c.set(m, 1, nd); "
                "x = c.res(9); c.set(m, 2, x); c.mark(m); "
                "a = [m]; k = a[0]; "
                "r = [c.tag(k), c.marked(k), c.get(k, 0)[1][0], c.same(c.get(k, 0), c.get(m, 0)), c.same(c.get(k, 1), nd), "
                "c.same(c.get(k, 2), x), c.cellid(k, 2) != c.cellid(m, 2)]; "
                "c.set(k, 2, nd); "
                "r + [c.same(c.get(m, 2), x)];",
                probe, arm);
    ASSERT_TRUE(o.finished) << o.described;
    EXPECT_EQ(o.described, "[30566, true, 2, false, true, true, true, true]");
  }
}

TEST(ValueTypes, ATypesCopyIsTheStoredValueAndAWrongAnswerIsHostFunctionFailed) {
  struct Row {
    int mode;
    const char * expected;
  };
  const Row rows[] = {
    {0, "[1042, 42, false]"},
    {4, "[42, 42, false]"},   // a decline: the default copy
  };
  for (const Row & row : rows) {
    Probe probe;
    probe.copy_mode = row.mode;
    Out o = run(USE "m = c.vcopy(42); a = [m]; [c.tag(a[0]), c.tag(m), c.same(a[0], m)];", probe);
    ASSERT_TRUE(o.finished) << o.described;
    EXPECT_EQ(o.described, row.expected) << "mode " << row.mode;
    EXPECT_EQ(probe.copy_calls, 1);
  }
  for (int mode : {1, 2, 3}) {
    Probe probe;
    probe.copy_mode = mode;
    Out o = run(USE "m = c.vcopy(42); a = [m]; a[0];", probe);
    ASSERT_TRUE(o.finished) << o.described;
    EXPECT_EQ(o.described, mode == 3 ? "Error: Divide by zero" : "Error: Host function failed") << "mode " << mode;
  }
}

TEST(ValueTypes, AValueTypeWithAFinalizerAndItsCopyAreEachFinalizedOnce) {
  Probe probe;
  Out o = run(USE "m = c.vcopy(8); a = [m, m]; m = null; a = null; c.collect(); c.finals();", probe, forced_torture());
  ASSERT_TRUE(o.finished) << o.described;
  EXPECT_EQ(o.integer, 3);
  std::multiset<int64_t> tags(probe.final_tags.begin(), probe.final_tags.end());
  EXPECT_EQ(tags.count(8), 1u);
  EXPECT_EQ(tags.count(1008), 2u);
}

TEST(ValueTypes, ACopyPastTheDepthLimitIsTheRecursionLimit) {
  if (tt::heavy_instruments()) {
    GTEST_SKIP() << "the chain is built by copies of every length up to 2,048, which the torture and valgrind arms would take hours over";
  }
  Probe probe;
  // A chain of 2,049 instances, each holding the one before in slot 0: every store copies the chain built so far,
  // which stays inside the limit (the copy's deepest instance is at depth 2,047). Storing the whole chain once more
  // copies 2,049 levels, one past it.
  Out o = run(USE "m = c.point(0); i = 1; while (i < 2049) { p = c.point(i); c.set(p, 0, m); m = p; i = i + 1; } "
              "t = [c.tag(m), c.tag(c.get(m, 0))]; x = [m]; [t, x[0]];", probe);
  ASSERT_TRUE(o.finished) << o.described;
  EXPECT_EQ(o.described.substr(0, 14), "[[2048, 2047],") << o.described;
  EXPECT_NE(o.described.find("Recursion Limit Exceeded"), std::string::npos) << o.described;
}

TEST(ValueTypes, EveryMemoryBudgetGivesTheCopyOrOutOfMemoryAndLeaksNothing) {
  // A value type with 64 KiB of bytes (so that its copy needs memory the heap must ask the budget for) and a
  // container in its slot, stored once: swept over memory budgets from a quarter of the run's peak to past it, with
  // no reserve. Once the instance is made (the output says so), every run ends with
  // the copy or with Out of memory (a budget that runs out before that, even in `use c`, can fail any way the
  // program's earlier statements then fail); the harness finds nothing leaked in any; and some budget runs out after the instance was made.
  const char * source = USE "m = c.bigv(7); c.set(m, 0, [1, [2, 3], {k: 4}]); if (c.tag(m) == 7) { print(\"made;\"); } a = [m]; a[0];";
  Probe measuring;
  Out full = run(source, measuring);
  ASSERT_TRUE(full.finished) << full.described;
  ASSERT_EQ(full.kind, GLTANG_KIND_CUSTOM) << full.described;
  bool copied_once = false;
  bool ran_out_after_making = false;
  // From a quarter of the peak (a relocating heap's peak counts the copies it moves through), in 256 steps.
  const uint64_t step = full.memory_peak / 256u > 512u ? full.memory_peak / 256u : 512u;
  for (uint64_t budget = full.memory_peak / 4u; budget <= full.memory_peak + 4096u; budget += step) {
    Probe probe;
    Config config;
    config.memory_bytes = budget;
    config.memory_reserve = 0;
    Out o = run(source, probe, config);
    const bool copied = o.kind == GLTANG_KIND_CUSTOM;
    const bool out_of_memory = o.error == GLTANG_ERROR_OUT_OF_MEMORY;
    if (o.raw.find("made;") != std::string::npos) {
      EXPECT_TRUE(copied || out_of_memory || !o.finished) << "memory " << budget << ": " << o.described;
      ran_out_after_making = ran_out_after_making || out_of_memory;
    }
    copied_once = copied_once || copied;
  }
  EXPECT_TRUE(copied_once) << "the sweep never reached a budget the run fits in";
  EXPECT_TRUE(ran_out_after_making) << "no budget ran out while the instance was copied";
}

TEST(ValueTypes, CompiledCodeStoresTheSameCopiesAsTheInterpreter) {
  GLTANG_REQUIRE_JIT_BACKEND();
  const char * source = USE
      "function f(m, n) { use c; a = [m]; c.settag(m, n); return c.tag(a[0]) * 1000 + c.tag(m); }\n"
      "function g(r, n) { use c; a = [r]; c.settag(r, n); return c.tag(a[0]); }\n"
      "function h(v) { use c; a = [v]; return c.tag(a[0]); }\n"
      "s = 0; i = 0; m = c.point(1); r = c.node(); v = c.vcopy(42);\n"
      "while (i < 40) { s = s + f(m, i); s = s + g(r, i); s = s + h(v); i = i + 1; }\n"
      "s;";
  Config interpreted;
  interpreted.jit_threshold = 0;
  Config compiled;
  compiled.jit_threshold = 1;
  Probe pi, pc;
  Out a = run(source, pi, interpreted);
  Out b = run(source, pc, compiled);
  ASSERT_TRUE(a.finished) << a.described;
  ASSERT_TRUE(b.finished) << b.described;
  EXPECT_EQ(a.described, b.described);
  EXPECT_GT(b.jit.entries, 0u) << "nothing ran compiled: the differential compares the interpreter with itself";
  EXPECT_GT(b.jit.functions_compiled, 0u);
  // h's copy is vcopy's own `copy` (a callback, from compiled code's ADOPT in the JIT run): its tag plus 1000.
  EXPECT_EQ(pi.copy_calls, 40);
  EXPECT_EQ(pc.copy_calls, 40);
  // f's array holds a copy of m made before m's tag is set to n, so it has the tag the call before set (1 at
  // first); g's array holds r itself, whose tag is n by the time it is read.
  int64_t expected = 0;
  int64_t previous = 1;
  for (int64_t i = 0; i < 40; ++i) {
    expected += previous * 1000 + i;
    previous = i;
    expected += i;
    expected += 1042;
  }
  EXPECT_EQ(b.integer, expected);
}

TEST(ValueTypes, AnOldDescriptorIsAReferenceTypeWithStrongSlotsAndNoFinalizer) {
  Probe probe;
  // Registered with the size of the descriptor before this story's members, and garbage where they would be.
  GLTANG_TypeRegistry * r = nullptr;
  ASSERT_EQ(gltang_type_registry_create(&r), GLTANG_OK);
  GLTANG_TypeDescriptor d = GLTANG_TYPE_DESCRIPTOR_INIT;
  d.name = "old";
  d.slot_count = 1;
  d.byte_count = 8;
  d.thread_safe = true;
  d.size = offsetof(GLTANG_TypeDescriptor, copy_semantics);
  d.copy_semantics = GLTANG_COPY_VALUE;
  d.finalizer = finalize;
  d.weak_slots = node_weak;
  d.weak_slot_count = 2;
  const GLTANG_Type * old = nullptr;
  ASSERT_EQ(gltang_type_registry_add(r, &d, &old), GLTANG_OK);
  static const GLTANG_Type * old_type = nullptr;
  old_type = old;
  Out o = run(USE "use o; m = o.old(); a = [m]; c.set(m, 0, c.node()); c.collect(); [c.same(a[0], m), c.get(m, 0) == null, c.finals()];", probe, Config(),
              [&](Context & context, Probe &) {
                ASSERT_EQ(gltang_execution_attach_types(context.execution, r), GLTANG_OK);
                GLTANG_Library * lib = nullptr;
                ASSERT_EQ(gltang_library_create("o", &lib), GLTANG_OK);
                ASSERT_EQ(gltang_library_add_native(lib, "old", [](GLTANG_NativeCall * call, void *) { return make_tagged(call, old_type); }, nullptr), GLTANG_OK);
                ASSERT_EQ(gltang_library_add_library(context.library(), lib), GLTANG_OK);
                gltang_library_release(lib);
              });
  gltang_type_registry_release(r);
  ASSERT_TRUE(o.finished) << o.described;
  EXPECT_EQ(o.described, "[true, false, 0]");
}

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

TEST(Identity, AnIdFindsItsInstanceAfterItMovesAndIsRefusedOnceItDies) {
  for (const Config & arm : collector_arms()) {
    Probe probe;
    Out o = run(USE "x = c.node(); y = c.node(); ix = c.id(x); iy = c.id(y); h = c.ihash(x); "
                "i = 0; while (i < 20) { [i]; i = i + 1; } c.collect(); "
                "r = [ix > 0, iy != ix, c.id(x) == ix, c.same(c.fromid(ix), x), c.same(c.fromid(iy), y), c.ihash(x) == h]; "
                "y = null; c.collect(); "
                "r + [c.fromid(iy), c.fromid(ix + iy + 1000), c.fromid(0)];",
                probe, arm);
    ASSERT_TRUE(o.finished) << o.described;
    const std::string refused = std::to_string(-static_cast<int64_t>(GLTANG_ERR_INVALID));
    EXPECT_EQ(o.described, "[true, true, true, true, true, true, " + refused + ", " + refused + ", " + refused + "]");
    if (relocation_requested() && arm.torture == 1) {
      EXPECT_GT(o.moved, 0u);
    }
  }
}

TEST(Identity, AnIdOfAnotherKindOfObjectIsNotAnInstance) {
  Probe probe;
  Out o = run(USE "use probe_id; a = [1]; probe_id(a);", probe, Config(), [&](Context & context, Probe & p) {
    ASSERT_EQ(gltang_library_add_native(context.library(), "probe_id", [](GLTANG_NativeCall * call, void * user) {
      GLTANG_Handle a, out;
      EXPECT_EQ(gltang_call_arg(call, 0, &a), GLTANG_OK);
      uint64_t id = 0;
      EXPECT_EQ(grheap_id_of(probe_of(user)->context->heap, gltang_vm_test_handle_object(call, a), &id), GRHEAP_OK);
      EXPECT_EQ(gltang_handle_custom_from_id(call, id, &out), GLTANG_ERR_INVALID);
      EXPECT_EQ(gltang_handle_custom_id(call, a, &id), GLTANG_ERR_INVALID);
      gltang_call_return_integer(call, 1);
      return true;
    }, &p), GLTANG_OK);
  });
  ASSERT_TRUE(o.finished) << o.described;
  EXPECT_EQ(o.integer, 1);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  tt::install_watchdog(600);
  return RUN_ALL_TESTS();
}
