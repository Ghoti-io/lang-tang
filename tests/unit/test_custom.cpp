// Host-defined value types (type.h): the descriptor and its registry, the
// instance and its slots and bytes, and every operation of the language that
// falls through to a type's table, with ctang's reflection rule (spec
// spec-tang-host-extensibility story 5).
//
// The types under test:
//   money  raw bytes: an int64 amount; one slot, its label. Implements every
//          operation; it declines an operand it has no rule for.
//   plain  two slots, no bytes, no operation at all: every operation is the
//          language's error, and equality is identity.
//   shy    implements every operation and declines each one.
//   echo   its add answers `self`, which makes nothing; the rest decline.
// Each records what it was called with in the log, so a test states which side
// was asked, in what order, with which `self_is_lhs` and `is_assignment`.
//
// Tests that depend on the collector force its arm on inside the test, so that
// a plain `make test` runs them too; the suite is also in TORTURE_BOUNDED.

#include "exec_harness.h"

#include <ghoti.io/lang-tang/bytecode.h>
#include <ghoti.io/lang-tang/type.h>
#include <ghoti.io/runtime-heap/id.h>

#include <atomic>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using tt::Compiled;
using tt::Config;
using tt::Context;

namespace {

struct Types;

struct Types {
  GLTANG_TypeRegistry * registry = nullptr;
  const GLTANG_Type * money = nullptr;
  const GLTANG_Type * plain = nullptr;
  const GLTANG_Type * shy = nullptr;
  const GLTANG_Type * echo = nullptr;
  std::vector<std::string> log;
  Context * context = nullptr;           ///< For the probes; set by the run.
  std::vector<uint64_t> tracked;         ///< Stable IDs of objects a test watches.
  /// Thread-safe unless a test says otherwise: a registry of thread-unsafe
  /// types is bound to the first context it is attached to, and most tests here
  /// run one set of types in several contexts, one after another.
  explicit Types(bool thread_safe = true);
  ~Types() { gltang_type_registry_release(registry); }
  Types(const Types &) = delete;
  Types & operator=(const Types &) = delete;
};

Types * types_of(void * user) {
  return static_cast<Types *>(user);
}

const char * flag(bool b) {
  return b ? "1" : "0";
}

// ---------------------------------------------------------------------------
// money
// ---------------------------------------------------------------------------

bool amount_of(GLTANG_NativeCall * call, GLTANG_Handle h, int64_t * out) {
  return gltang_handle_custom_read_bytes(call, h, 0, out, sizeof(*out)) == GLTANG_OK;
}

/// The other operand as an amount: a money's, or an integer; false for anything else.
bool other_amount(GLTANG_NativeCall * call, Types * t, GLTANG_Handle other, int64_t * out) {
  if (gltang_type_same(gltang_handle_custom_type(call, other), t->money)) {
    return amount_of(call, other, out);
  }
  if (gltang_handle_kind(call, other) == GLTANG_KIND_INTEGER) {
    *out = gltang_handle_integer(call, other);
    return true;
  }
  return false;
}

GLTANG_OpStatus answer_money(GLTANG_NativeCall * call, Types * t, int64_t amount) {
  GLTANG_Handle h;
  if (gltang_handle_make_custom(call, t->money, &h) != GLTANG_OK ||
      gltang_handle_custom_write_bytes(call, h, 0, &amount, sizeof amount) != GLTANG_OK) {
    return GLTANG_OP_ERROR;
  }
  gltang_call_return_handle(call, h);
  return GLTANG_OP_OK;
}

GLTANG_OpStatus money_add(GLTANG_NativeCall * call, void * user, GLTANG_Handle self, GLTANG_Handle other, bool lhs, bool assign) {
  Types * t = types_of(user);
  t->log.push_back(std::string("money add lhs=") + flag(lhs) + " assign=" + flag(assign));
  int64_t a = 0, b = 0;
  amount_of(call, self, &a);
  if (gltang_handle_kind(call, other) == GLTANG_KIND_STRING) {
    // A rule of its own for a string: its length and the amount.
    gltang_call_return_integer(call, a + static_cast<int64_t>(gltang_handle_string_graphemes(call, other)));
    return GLTANG_OP_OK;
  }
  if (!other_amount(call, t, other, &b)) {
    return GLTANG_OP_DECLINE;
  }
  return answer_money(call, t, a + b);
}

/// The binary operators other than add: `f(x, y)` where x is the left operand.
GLTANG_OpStatus money_arith(GLTANG_NativeCall * call, void * user, GLTANG_Handle self, GLTANG_Handle other, bool lhs, bool assign,
                            const char * name, const std::function<bool(int64_t, int64_t, int64_t *)> & f) {
  Types * t = types_of(user);
  t->log.push_back(std::string("money ") + name + " lhs=" + flag(lhs) + " assign=" + flag(assign));
  int64_t a = 0, b = 0, r = 0;
  amount_of(call, self, &a);
  if (!other_amount(call, t, other, &b)) {
    return GLTANG_OP_DECLINE;
  }
  // The reflected call computes `other OP self`.
  if (!(lhs ? f(a, b, &r) : f(b, a, &r))) {
    gltang_call_return_error(call, GLTANG_ERROR_DIVIDE_BY_ZERO);
    return GLTANG_OP_ERROR;
  }
  return answer_money(call, t, r);
}

GLTANG_OpStatus money_sub(GLTANG_NativeCall * c, void * u, GLTANG_Handle s, GLTANG_Handle o, bool l, bool a) {
  return money_arith(c, u, s, o, l, a, "sub", [](int64_t x, int64_t y, int64_t * r) { *r = x - y; return true; });
}
GLTANG_OpStatus money_mul(GLTANG_NativeCall * c, void * u, GLTANG_Handle s, GLTANG_Handle o, bool l, bool a) {
  return money_arith(c, u, s, o, l, a, "mul", [](int64_t x, int64_t y, int64_t * r) { *r = x * y; return true; });
}
GLTANG_OpStatus money_div(GLTANG_NativeCall * c, void * u, GLTANG_Handle s, GLTANG_Handle o, bool l, bool a) {
  return money_arith(c, u, s, o, l, a, "div", [](int64_t x, int64_t y, int64_t * r) { if (!y) { return false; } *r = x / y; return true; });
}
GLTANG_OpStatus money_mod(GLTANG_NativeCall * c, void * u, GLTANG_Handle s, GLTANG_Handle o, bool l, bool a) {
  return money_arith(c, u, s, o, l, a, "mod", [](int64_t x, int64_t y, int64_t * r) { if (!y) { return false; } *r = x % y; return true; });
}

GLTANG_OpStatus money_neg(GLTANG_NativeCall * call, void * user, GLTANG_Handle self) {
  Types * t = types_of(user);
  t->log.push_back("money neg");
  int64_t a = 0;
  amount_of(call, self, &a);
  return answer_money(call, t, -a);
}

GLTANG_OpStatus money_compare(GLTANG_NativeCall * call, void * user, GLTANG_Handle self, GLTANG_Handle other, bool lhs, const char * name,
                              const std::function<bool(int64_t, int64_t)> & f) {
  Types * t = types_of(user);
  t->log.push_back(std::string("money ") + name + " lhs=" + flag(lhs));
  int64_t a = 0, b = 0;
  amount_of(call, self, &a);
  if (!other_amount(call, t, other, &b)) {
    return GLTANG_OP_DECLINE;
  }
  gltang_call_return_bool(call, lhs ? f(a, b) : f(b, a));
  return GLTANG_OP_OK;
}

GLTANG_OpStatus money_lt(GLTANG_NativeCall * c, void * u, GLTANG_Handle s, GLTANG_Handle o, bool l) {
  return money_compare(c, u, s, o, l, "lt", [](int64_t x, int64_t y) { return x < y; });
}
GLTANG_OpStatus money_le(GLTANG_NativeCall * c, void * u, GLTANG_Handle s, GLTANG_Handle o, bool l) {
  return money_compare(c, u, s, o, l, "le", [](int64_t x, int64_t y) { return x <= y; });
}
GLTANG_OpStatus money_gt(GLTANG_NativeCall * c, void * u, GLTANG_Handle s, GLTANG_Handle o, bool l) {
  return money_compare(c, u, s, o, l, "gt", [](int64_t x, int64_t y) { return x > y; });
}
GLTANG_OpStatus money_ge(GLTANG_NativeCall * c, void * u, GLTANG_Handle s, GLTANG_Handle o, bool l) {
  return money_compare(c, u, s, o, l, "ge", [](int64_t x, int64_t y) { return x >= y; });
}
GLTANG_OpStatus money_eq(GLTANG_NativeCall * c, void * u, GLTANG_Handle s, GLTANG_Handle o, bool l) {
  return money_compare(c, u, s, o, l, "eq", [](int64_t x, int64_t y) { return x == y; });
}

bool name_is(GLTANG_NativeCall * call, GLTANG_Handle name, const char * text) {
  size_t length = 0;
  const char * n = gltang_handle_text(call, name, &length);
  return n && length == std::strlen(text) && !std::memcmp(n, text, length);
}

GLTANG_OpStatus money_get(GLTANG_NativeCall * call, void * user, GLTANG_Handle self, GLTANG_Handle name) {
  Types * t = types_of(user);
  t->log.push_back("money get");
  if (name_is(call, name, "amount")) {
    int64_t a = 0;
    amount_of(call, self, &a);
    gltang_call_return_integer(call, a);
    return GLTANG_OP_OK;
  }
  if (name_is(call, name, "label")) {
    GLTANG_Handle v;
    if (gltang_handle_custom_slot_get(call, self, 0, &v) != GLTANG_OK) {
      return GLTANG_OP_ERROR;
    }
    gltang_call_return_handle(call, v);
    return GLTANG_OP_OK;
  }
  return GLTANG_OP_DECLINE;
}

GLTANG_OpStatus money_set(GLTANG_NativeCall * call, void * user, GLTANG_Handle self, GLTANG_Handle name, GLTANG_Handle value) {
  Types * t = types_of(user);
  t->log.push_back("money set");
  if (name_is(call, name, "amount") && gltang_handle_kind(call, value) == GLTANG_KIND_INTEGER) {
    int64_t a = gltang_handle_integer(call, value);
    return gltang_handle_custom_write_bytes(call, self, 0, &a, sizeof a) == GLTANG_OK ? GLTANG_OP_OK : GLTANG_OP_ERROR;
  }
  if (name_is(call, name, "label")) {
    return gltang_handle_custom_slot_set(call, self, 0, value) == GLTANG_OK ? GLTANG_OP_OK : GLTANG_OP_ERROR;
  }
  return GLTANG_OP_DECLINE;
}

GLTANG_OpStatus money_index(GLTANG_NativeCall * call, void * user, GLTANG_Handle self, GLTANG_Handle index) {
  (void)self;
  types_of(user)->log.push_back("money index");
  // Any value is an index: the answer is the index itself.
  gltang_call_return_handle(call, index);
  return GLTANG_OP_OK;
}

GLTANG_OpStatus money_set_index(GLTANG_NativeCall * call, void * user, GLTANG_Handle self, GLTANG_Handle index, GLTANG_Handle value) {
  (void)index;
  types_of(user)->log.push_back("money set_index");
  return gltang_handle_custom_slot_set(call, self, 0, value) == GLTANG_OK ? GLTANG_OP_OK : GLTANG_OP_ERROR;
}

GLTANG_OpStatus money_slice(GLTANG_NativeCall * call, void * user, GLTANG_Handle self, GLTANG_Handle a, GLTANG_Handle b, GLTANG_Handle c) {
  (void)self;
  types_of(user)->log.push_back("money slice");
  GLTANG_Handle array;
  if (gltang_handle_make_array(call, 3, &array) != GLTANG_OK || gltang_handle_array_push(call, array, a) != GLTANG_OK ||
      gltang_handle_array_push(call, array, b) != GLTANG_OK || gltang_handle_array_push(call, array, c) != GLTANG_OK) {
    return GLTANG_OP_ERROR;
  }
  gltang_call_return_handle(call, array);
  return GLTANG_OP_OK;
}

// ---------------------------------------------------------------------------
// shy and echo
// ---------------------------------------------------------------------------

GLTANG_OpStatus shy_binary(GLTANG_NativeCall *, void * user, GLTANG_Handle, GLTANG_Handle, bool lhs, bool) {
  types_of(user)->log.push_back(std::string("shy binary lhs=") + flag(lhs));
  return GLTANG_OP_DECLINE;
}
GLTANG_OpStatus shy_compare(GLTANG_NativeCall *, void * user, GLTANG_Handle, GLTANG_Handle, bool lhs) {
  types_of(user)->log.push_back(std::string("shy compare lhs=") + flag(lhs));
  return GLTANG_OP_DECLINE;
}
GLTANG_OpStatus shy_unary(GLTANG_NativeCall *, void * user, GLTANG_Handle) {
  types_of(user)->log.push_back("shy unary");
  return GLTANG_OP_DECLINE;
}
GLTANG_OpStatus shy_get(GLTANG_NativeCall *, void * user, GLTANG_Handle, GLTANG_Handle) {
  types_of(user)->log.push_back("shy get");
  return GLTANG_OP_DECLINE;
}
GLTANG_OpStatus shy_set(GLTANG_NativeCall *, void * user, GLTANG_Handle, GLTANG_Handle, GLTANG_Handle) {
  types_of(user)->log.push_back("shy set");
  return GLTANG_OP_DECLINE;
}
GLTANG_OpStatus shy_slice(GLTANG_NativeCall *, void * user, GLTANG_Handle, GLTANG_Handle, GLTANG_Handle, GLTANG_Handle) {
  types_of(user)->log.push_back("shy slice");
  return GLTANG_OP_DECLINE;
}

GLTANG_OpStatus echo_add(GLTANG_NativeCall * call, void * user, GLTANG_Handle self, GLTANG_Handle, bool lhs, bool) {
  types_of(user)->log.push_back(std::string("echo add lhs=") + flag(lhs));
  gltang_call_return_handle(call, self);
  return GLTANG_OP_OK;
}

Types::Types(bool thread_safe) {
  EXPECT_EQ(gltang_type_registry_create(&registry), GLTANG_OK);
  GLTANG_TypeDescriptor d = GLTANG_TYPE_DESCRIPTOR_INIT;
  d.name = "money";
  d.version = 1;
  d.slot_count = 1;
  d.byte_count = sizeof(int64_t);
  d.thread_safe = thread_safe;
  d.user = this;
  d.add = money_add;
  d.subtract = money_sub;
  d.multiply = money_mul;
  d.divide = money_div;
  d.modulo = money_mod;
  d.negate = money_neg;
  d.less = money_lt;
  d.less_equal = money_le;
  d.greater = money_gt;
  d.greater_equal = money_ge;
  d.equal = money_eq;
  d.attribute_get = money_get;
  d.attribute_set = money_set;
  d.index_get = money_index;
  d.index_set = money_set_index;
  d.slice = money_slice;
  EXPECT_EQ(gltang_type_registry_add(registry, &d, &money), GLTANG_OK);

  GLTANG_TypeDescriptor p = GLTANG_TYPE_DESCRIPTOR_INIT;
  p.name = "plain";
  p.slot_count = 2;
  p.thread_safe = thread_safe;
  EXPECT_EQ(gltang_type_registry_add(registry, &p, &plain), GLTANG_OK);

  GLTANG_TypeDescriptor s = GLTANG_TYPE_DESCRIPTOR_INIT;
  s.name = "shy";
  s.thread_safe = thread_safe;
  s.user = this;
  s.add = s.subtract = s.multiply = s.divide = s.modulo = shy_binary;
  s.negate = shy_unary;
  s.less = s.less_equal = s.greater = s.greater_equal = s.equal = s.not_equal = shy_compare;
  s.attribute_get = shy_get;
  s.attribute_set = shy_set;
  s.index_get = shy_get;
  s.index_set = shy_set;
  s.slice = shy_slice;
  EXPECT_EQ(gltang_type_registry_add(registry, &s, &shy), GLTANG_OK);

  GLTANG_TypeDescriptor e = GLTANG_TYPE_DESCRIPTOR_INIT;
  e.name = "echo";
  e.thread_safe = thread_safe;
  e.user = this;
  e.add = echo_add;
  EXPECT_EQ(gltang_type_registry_add(registry, &e, &echo), GLTANG_OK);
}

// ---------------------------------------------------------------------------
// The natives a program makes and reads instances with
// ---------------------------------------------------------------------------

bool make_money(GLTANG_NativeCall * call, void * user) {
  Types * t = types_of(user);
  int64_t amount = gltang_call_integer(call, 0);
  GLTANG_Handle h;
  if (gltang_handle_make_custom(call, t->money, &h) != GLTANG_OK) {
    return false;
  }
  EXPECT_EQ(gltang_handle_custom_write_bytes(call, h, 0, &amount, sizeof amount), GLTANG_OK);
  gltang_call_return_handle(call, h);
  return true;
}

template <const GLTANG_Type * Types::*Member>
bool make_plain_like(GLTANG_NativeCall * call, void * user) {
  GLTANG_Handle h;
  if (gltang_handle_make_custom(call, types_of(user)->*Member, &h) != GLTANG_OK) {
    return false;
  }
  gltang_call_return_handle(call, h);
  return true;
}

/// setslot(x, i, v): slot i of x is v.
bool set_slot(GLTANG_NativeCall * call, void *) {
  GLTANG_Handle x, v;
  if (gltang_call_arg(call, 0, &x) != GLTANG_OK || gltang_call_arg(call, 2, &v) != GLTANG_OK) {
    return false;
  }
  return gltang_handle_custom_slot_set(call, x, static_cast<size_t>(gltang_call_integer(call, 1)), v) == GLTANG_OK;
}

/// slot(x, i): what slot i of x holds.
bool get_slot(GLTANG_NativeCall * call, void *) {
  GLTANG_Handle x, v;
  if (gltang_call_arg(call, 0, &x) != GLTANG_OK ||
      gltang_handle_custom_slot_get(call, x, static_cast<size_t>(gltang_call_integer(call, 1)), &v) != GLTANG_OK) {
    return false;
  }
  gltang_call_return_handle(call, v);
  return true;
}

/// allocs(): the objects the heap has ever made.
bool allocs(GLTANG_NativeCall * call, void * user) {
  GRHEAP_Stats stats;
  EXPECT_EQ(grheap_stats(types_of(user)->context->heap, &stats), GRHEAP_OK);
  gltang_call_return_integer(call, static_cast<int64_t>(stats.allocations));
  return true;
}

/// mallocs(): the blocks the context's allocator has been asked for.
bool mallocs(GLTANG_NativeCall * call, void * user) {
  gltang_call_return_integer(call, types_of(user)->context->tracker.calls);
  return true;
}

/// track(x): remembers x's stable ID.
bool track(GLTANG_NativeCall * call, void * user) {
  Types * t = types_of(user);
  GLTANG_Handle x;
  if (gltang_call_arg(call, 0, &x) != GLTANG_OK) {
    return false;
  }
  uint64_t id = 0;
  EXPECT_EQ(grheap_id_of(t->context->heap, gltang_vm_test_handle_object(call, x), &id), GRHEAP_OK);
  t->tracked.push_back(id);
  gltang_call_return_null(call);
  return true;
}

/// alive(): collects, and answers how many tracked objects are still alive.
bool alive(GLTANG_NativeCall * call, void * user) {
  Types * t = types_of(user);
  EXPECT_EQ(grheap_collect(t->context->heap), GRHEAP_OK);
  int64_t n = 0;
  for (uint64_t id : t->tracked) {
    void * object = nullptr;
    n += grheap_object_of_id(t->context->heap, id, &object) == GRHEAP_OK;
  }
  gltang_call_return_integer(call, n);
  return true;
}

struct Out {
  bool finished = false;
  GRCORE_Result ran = GRCORE_OK;
  std::string described;
  std::string raw;
  GLTANG_ValueKind kind = GLTANG_KIND_NULL;
  GLTANG_ErrorKind error = GLTANG_ERROR_KIND_COUNT;
  int64_t integer = 0;
  bool boolean = false;
  size_t errors_listed = 0;
  std::vector<std::string> log;
};

using Setup = std::function<void(Context &, Types &)>;

/// Runs `source` with the four types' natives under `use t;` and the registry attached.
Out run(const std::string & source, Types & types, const Config & config = Config(), const Setup & setup = Setup()) {
  Out out;
  Compiled compiled(source);
  EXPECT_TRUE(compiled.ok()) << source << ": " << compiled.error.message;
  if (!compiled.ok()) {
    return out;
  }
  Context context(compiled.program, config);
  EXPECT_TRUE(context.ok());
  if (!context.ok()) {
    return out;
  }
  types.context = &context;
  types.log.clear();
  types.tracked.clear();
  GLTANG_Library * t = nullptr;
  EXPECT_EQ(gltang_library_create("t", &t), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_native(t, "money", make_money, &types), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_native(t, "plain", make_plain_like<&Types::plain>, &types), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_native(t, "shy", make_plain_like<&Types::shy>, &types), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_native(t, "echo", make_plain_like<&Types::echo>, &types), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_native(t, "setslot", set_slot, &types), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_native(t, "slot", get_slot, &types), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_native(t, "allocs", allocs, &types), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_native(t, "mallocs", mallocs, &types), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_native(t, "track", track, &types), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_native(t, "alive", alive, &types), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_library(context.library(), t), GLTANG_OK);
  gltang_library_release(t);
  EXPECT_EQ(gltang_execution_attach_types(context.execution, types.registry), GLTANG_OK);
  if (setup) {
    setup(context, types);
  }
  out.finished = context.execute();
  out.ran = context.ran;
  out.raw = context.raw();
  out.described = context.describe();
  out.kind = context.kind();
  out.errors_listed = context.error_count();
  if (context.is_error()) {
    out.error = context.error_kind();
  }
  if (context.is_integer()) {
    out.integer = context.integer();
  }
  if (context.is_bool()) {
    out.boolean = context.boolean();
  }
  out.log = types.log;
  types.context = nullptr;
  return out;
}

using Log = std::vector<std::string>;

Config forced_torture() {
  Config c;
  c.torture = 1;
  c.verify = 1;
  return c;
}

#define PRELUDE "use t; money = t.money; plain = t.plain; shy = t.shy; echo = t.echo; "

}  // namespace

// ---------------------------------------------------------------------------
// The registry
// ---------------------------------------------------------------------------

TEST(TypeRegistry, AddCopiesTheDescriptorSoTheHostsStructNeedNotLive) {
  GLTANG_TypeRegistry * r = nullptr;
  ASSERT_EQ(gltang_type_registry_create(&r), GLTANG_OK);
  char name[] = "money";
  GLTANG_TypeDescriptor d = GLTANG_TYPE_DESCRIPTOR_INIT;
  d.name = name;
  d.version = 3;
  d.slot_count = 2;
  d.byte_count = 16;
  int user = 0;
  d.user = &user;
  const GLTANG_Type * type = nullptr;
  ASSERT_EQ(gltang_type_registry_add(r, &d, &type), GLTANG_OK);
  std::memset(name, 'x', sizeof name - 1);
  std::memset(&d, 0, sizeof d);
  EXPECT_STREQ(gltang_type_name(type), "money");
  EXPECT_EQ(gltang_type_version(type), 3u);
  EXPECT_EQ(gltang_type_slot_count(type), 2u);
  EXPECT_EQ(gltang_type_byte_count(type), 16u);
  EXPECT_EQ(gltang_type_user(type), &user);
  EXPECT_EQ(gltang_type_registry(type), r);
  EXPECT_EQ(gltang_type_registry_count(r), 1u);
  EXPECT_EQ(gltang_type_registry_find(r, "money", 3), type);
  EXPECT_EQ(gltang_type_registry_find(r, "money", 2), nullptr);
  gltang_type_registry_release(r);
}

TEST(TypeRegistry, RefusesADescriptorItCannotHold) {
  GLTANG_TypeRegistry * r = nullptr;
  ASSERT_EQ(gltang_type_registry_create(&r), GLTANG_OK);
  auto add = [&](const std::function<void(GLTANG_TypeDescriptor &)> & change) {
    GLTANG_TypeDescriptor d = GLTANG_TYPE_DESCRIPTOR_INIT;
    d.name = "ok";
    change(d);
    return gltang_type_registry_add(r, &d, nullptr);
  };
  EXPECT_EQ(gltang_type_registry_add(nullptr, nullptr, nullptr), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_type_registry_add(r, nullptr, nullptr), GLTANG_ERR_INVALID);
  EXPECT_EQ(add([](GLTANG_TypeDescriptor & d) { d.size = GLTANG_TYPE_DESCRIPTOR_MIN_SIZE - 1; }), GLTANG_ERR_INVALID);
  EXPECT_EQ(add([](GLTANG_TypeDescriptor & d) { d.size = 0; }), GLTANG_ERR_INVALID);
  EXPECT_EQ(add([](GLTANG_TypeDescriptor & d) { d.name = nullptr; }), GLTANG_ERR_INVALID);
  EXPECT_EQ(add([](GLTANG_TypeDescriptor & d) { d.name = ""; }), GLTANG_ERR_INVALID);
  EXPECT_EQ(add([](GLTANG_TypeDescriptor & d) { d.name = "9lives"; }), GLTANG_ERR_INVALID);
  EXPECT_EQ(add([](GLTANG_TypeDescriptor & d) { d.name = "a.b"; }), GLTANG_ERR_INVALID);
  EXPECT_EQ(add([](GLTANG_TypeDescriptor & d) { d.name = "a b"; }), GLTANG_ERR_INVALID);
  static const std::string long64(64, 'n');
  static const std::string long65(65, 'n');
  EXPECT_EQ(add([](GLTANG_TypeDescriptor & d) { d.name = long64.c_str(); }), GLTANG_OK) << "64 characters is the most";
  EXPECT_EQ(add([](GLTANG_TypeDescriptor & d) { d.name = long65.c_str(); }), GLTANG_ERR_INVALID);
  EXPECT_EQ(add([](GLTANG_TypeDescriptor & d) { d.name = "slots"; d.slot_count = GLTANG_TYPE_SLOTS_MAX; }), GLTANG_OK);
  EXPECT_EQ(add([](GLTANG_TypeDescriptor & d) { d.name = "slots2"; d.slot_count = GLTANG_TYPE_SLOTS_MAX + 1u; }), GLTANG_ERR_INVALID);
  EXPECT_EQ(add([](GLTANG_TypeDescriptor & d) { d.name = "bytes"; d.byte_count = GLTANG_TYPE_BYTES_MAX; }), GLTANG_OK);
  EXPECT_EQ(add([](GLTANG_TypeDescriptor & d) { d.name = "bytes2"; d.byte_count = GLTANG_TYPE_BYTES_MAX + 1u; }), GLTANG_ERR_INVALID);
  EXPECT_EQ(add([](GLTANG_TypeDescriptor & d) { d.name = "_under_9"; }), GLTANG_OK);
  gltang_type_registry_release(r);
}

TEST(TypeRegistry, RefusesADuplicateNameAndVersionButNotAnotherVersion) {
  GLTANG_TypeRegistry * r = nullptr;
  ASSERT_EQ(gltang_type_registry_create(&r), GLTANG_OK);
  GLTANG_TypeDescriptor d = GLTANG_TYPE_DESCRIPTOR_INIT;
  d.name = "money";
  d.version = 1;
  EXPECT_EQ(gltang_type_registry_add(r, &d, nullptr), GLTANG_OK);
  EXPECT_EQ(gltang_type_registry_add(r, &d, nullptr), GLTANG_ERR_INVALID);
  d.version = 2;
  EXPECT_EQ(gltang_type_registry_add(r, &d, nullptr), GLTANG_OK);
  EXPECT_EQ(gltang_type_registry_count(r), 2u);
  gltang_type_registry_release(r);
}

TEST(TypeRegistry, AReadsOnlyThePrefixItsSizeCoversAndALargerSizeIsAccepted) {
  // A module compiled against a later header states a larger size; the members
  // this engine knows are read and the rest ignored.
  struct Later {
    GLTANG_TypeDescriptor d;
    void * added_later;
  } later;
  std::memset(&later, 0, sizeof later);
  later.d.size = sizeof later;
  later.d.name = "later";
  later.added_later = &later;
  GLTANG_TypeRegistry * r = nullptr;
  ASSERT_EQ(gltang_type_registry_create(&r), GLTANG_OK);
  const GLTANG_Type * type = nullptr;
  EXPECT_EQ(gltang_type_registry_add(r, &later.d, &type), GLTANG_OK);
  EXPECT_STREQ(gltang_type_name(type), "later");
  gltang_type_registry_release(r);
}

TEST(TypeRegistry, IdsAreUniqueAndNeverReused) {
  uint64_t last = 0;
  for (int i = 0; i < 16; ++i) {
    GLTANG_TypeRegistry * r = nullptr;
    ASSERT_EQ(gltang_type_registry_create(&r), GLTANG_OK);
    uint64_t id = gltang_type_registry_id(r);
    EXPECT_GT(id, last) << "an id is never given twice, even after its registry is freed";
    last = id;
    gltang_type_registry_release(r);
  }
  EXPECT_EQ(gltang_type_registry_id(nullptr), 0u);
}

TEST(TypeRegistry, IdentityIsTheWholeTupleAndNeverTheName) {
  GLTANG_TypeRegistry * a = nullptr;
  GLTANG_TypeRegistry * b = nullptr;
  ASSERT_EQ(gltang_type_registry_create(&a), GLTANG_OK);
  ASSERT_EQ(gltang_type_registry_create(&b), GLTANG_OK);
  GLTANG_TypeDescriptor d = GLTANG_TYPE_DESCRIPTOR_INIT;
  d.name = "money";
  d.version = 1;
  const GLTANG_Type * in_a = nullptr;
  const GLTANG_Type * in_b = nullptr;
  const GLTANG_Type * v2 = nullptr;
  ASSERT_EQ(gltang_type_registry_add(a, &d, &in_a), GLTANG_OK);
  ASSERT_EQ(gltang_type_registry_add(b, &d, &in_b), GLTANG_OK);
  d.version = 2;
  ASSERT_EQ(gltang_type_registry_add(a, &d, &v2), GLTANG_OK);
  EXPECT_TRUE(gltang_type_same(in_a, in_a));
  EXPECT_FALSE(gltang_type_same(in_a, in_b)) << "two registries holding money version 1 hold two types";
  EXPECT_FALSE(gltang_type_same(in_a, v2)) << "two versions are two types";
  EXPECT_FALSE(gltang_type_same(in_a, nullptr));
  EXPECT_TRUE(gltang_type_same(nullptr, nullptr));
  // The table hash is of what the type declares: the same in both registries.
  EXPECT_EQ(gltang_type_table_hash(in_a), gltang_type_table_hash(in_b));
  EXPECT_NE(gltang_type_table_hash(in_a), gltang_type_table_hash(v2));
  gltang_type_registry_release(a);
  gltang_type_registry_release(b);
}

TEST(TypeRegistry, TheTableHashCoversTheOperationsAndNeverAnAddress) {
  GLTANG_TypeRegistry * r = nullptr;
  ASSERT_EQ(gltang_type_registry_create(&r), GLTANG_OK);
  int u1 = 0, u2 = 0;
  GLTANG_TypeDescriptor d = GLTANG_TYPE_DESCRIPTOR_INIT;
  d.name = "one";
  d.user = &u1;
  d.add = money_add;
  const GLTANG_Type * one = nullptr;
  ASSERT_EQ(gltang_type_registry_add(r, &d, &one), GLTANG_OK);
  GLTANG_TypeRegistry * r2 = nullptr;
  ASSERT_EQ(gltang_type_registry_create(&r2), GLTANG_OK);
  // Another user pointer and another function at the same entry: the same hash.
  d.user = &u2;
  d.add = echo_add;
  const GLTANG_Type * same = nullptr;
  ASSERT_EQ(gltang_type_registry_add(r2, &d, &same), GLTANG_OK);
  EXPECT_EQ(gltang_type_table_hash(one), gltang_type_table_hash(same));
  // One more operation, or a slot, or thread_safe: another hash.
  d.name = "two";
  const GLTANG_Type * base = nullptr;
  ASSERT_EQ(gltang_type_registry_add(r2, &d, &base), GLTANG_OK);
  d.name = "two";
  d.version = 9;
  d.negate = money_neg;
  const GLTANG_Type * more = nullptr;
  ASSERT_EQ(gltang_type_registry_add(r2, &d, &more), GLTANG_OK);
  EXPECT_NE(gltang_type_table_hash(base), gltang_type_table_hash(one)) << "the name is in the hash";
  GLTANG_TypeDescriptor e = GLTANG_TYPE_DESCRIPTOR_INIT;
  e.name = "three";
  const GLTANG_Type * bare = nullptr;
  ASSERT_EQ(gltang_type_registry_add(r2, &e, &bare), GLTANG_OK);
  e.name = "three";
  e.version = 1;
  e.thread_safe = true;
  const GLTANG_Type * safe = nullptr;
  ASSERT_EQ(gltang_type_registry_add(r2, &e, &safe), GLTANG_OK);
  e.version = 2;
  e.thread_safe = false;
  e.add = money_add;
  const GLTANG_Type * adds = nullptr;
  ASSERT_EQ(gltang_type_registry_add(r2, &e, &adds), GLTANG_OK);
  e.version = 0;
  e.add = nullptr;
  (void)more;
  // bare and a type of the same name and version that differs only in one
  // declared property: compared in a fresh registry each.
  auto hash_with = [](const std::function<void(GLTANG_TypeDescriptor &)> & change) {
    GLTANG_TypeRegistry * x = nullptr;
    EXPECT_EQ(gltang_type_registry_create(&x), GLTANG_OK);
    GLTANG_TypeDescriptor f = GLTANG_TYPE_DESCRIPTOR_INIT;
    f.name = "three";
    change(f);
    const GLTANG_Type * t = nullptr;
    EXPECT_EQ(gltang_type_registry_add(x, &f, &t), GLTANG_OK);
    uint64_t h = gltang_type_table_hash(t);
    gltang_type_registry_release(x);
    return h;
  };
  uint64_t h0 = gltang_type_table_hash(bare);
  EXPECT_EQ(hash_with([](GLTANG_TypeDescriptor &) {}), h0);
  EXPECT_NE(hash_with([](GLTANG_TypeDescriptor & f) { f.thread_safe = true; }), h0);
  EXPECT_NE(hash_with([](GLTANG_TypeDescriptor & f) { f.slot_count = 1; }), h0);
  EXPECT_NE(hash_with([](GLTANG_TypeDescriptor & f) { f.byte_count = 1; }), h0);
  EXPECT_NE(hash_with([](GLTANG_TypeDescriptor & f) { f.slice = money_slice; }), h0);
  EXPECT_NE(hash_with([](GLTANG_TypeDescriptor & f) { f.equal = money_eq; }), hash_with([](GLTANG_TypeDescriptor & f) { f.not_equal = money_eq; }));
  gltang_type_registry_release(r);
  gltang_type_registry_release(r2);
}

TEST(TypeRegistry, AttachingSealsItAndASealedRegistryRefusesAdd) {
  Types types;
  EXPECT_FALSE(gltang_type_registry_sealed(types.registry));
  Out o = run(PRELUDE "1;", types);
  ASSERT_TRUE(o.finished);
  EXPECT_TRUE(gltang_type_registry_sealed(types.registry));
  GLTANG_TypeDescriptor d = GLTANG_TYPE_DESCRIPTOR_INIT;
  d.name = "late";
  EXPECT_EQ(gltang_type_registry_add(types.registry, &d, nullptr), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_type_registry_find(types.registry, "late", 0), nullptr);
}

TEST(TypeRegistry, AttachIsRefusedWhenTheRunHasStartedForNullAndFromAHostFunction) {
  Types types;
  Compiled compiled("1;");
  Context context(compiled.program);
  ASSERT_TRUE(context.ok());
  EXPECT_EQ(gltang_execution_attach_types(nullptr, types.registry), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_execution_attach_types(context.execution, nullptr), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_execution_attach_types(context.execution, types.registry), GLTANG_OK);
  EXPECT_EQ(gltang_execution_attach_types(context.execution, types.registry), GLTANG_OK) << "twice is once";
  ASSERT_TRUE(context.execute());
  GLTANG_TypeRegistry * other = nullptr;
  ASSERT_EQ(gltang_type_registry_create(&other), GLTANG_OK);
  EXPECT_EQ(gltang_execution_attach_types(context.execution, other), GLTANG_ERR_INVALID) << "the run has started";
  EXPECT_FALSE(gltang_type_registry_sealed(other)) << "a refused attach changes nothing";
  gltang_type_registry_release(other);
}

TEST(TypeRegistry, AThreadUnsafeRegistryIsBoundToTheFirstContextAndAThreadSafeOneIsNot) {
  Types unsafe(false);
  Types safe(true);
  Compiled compiled("1;");
  Context first(compiled.program);
  Context second(compiled.program);
  Context third(compiled.program);
  ASSERT_TRUE(first.ok() && second.ok() && third.ok());
  EXPECT_EQ(gltang_execution_attach_types(first.execution, unsafe.registry), GLTANG_OK);
  EXPECT_EQ(gltang_execution_attach_types(second.execution, unsafe.registry), GLTANG_ERR_INVALID);
  // (The two sets declare money version 1 differently, thread_safe being in the
  // table hash, so they are attached to contexts of their own.)
  EXPECT_EQ(gltang_execution_attach_types(second.execution, safe.registry), GLTANG_OK);
  EXPECT_EQ(gltang_execution_attach_types(third.execution, safe.registry), GLTANG_OK);
  EXPECT_EQ(gltang_execution_attach_types(first.execution, safe.registry), GLTANG_ERR_INVALID) << "money version 1 twice, with two table hashes";
}

TEST(TypeRegistry, TwoRegistriesWhoseNameAndVersionClashWithAnotherTableHashCannotBothBeAttached) {
  auto registry_with = [](bool with_add) {
    GLTANG_TypeRegistry * r = nullptr;
    EXPECT_EQ(gltang_type_registry_create(&r), GLTANG_OK);
    GLTANG_TypeDescriptor d = GLTANG_TYPE_DESCRIPTOR_INIT;
    d.name = "money";
    d.version = 1;
    d.thread_safe = true;
    if (with_add) {
      d.add = money_add;
    }
    EXPECT_EQ(gltang_type_registry_add(r, &d, nullptr), GLTANG_OK);
    return r;
  };
  GLTANG_TypeRegistry * a = registry_with(true);
  GLTANG_TypeRegistry * same = registry_with(true);
  GLTANG_TypeRegistry * other = registry_with(false);
  Compiled compiled("1;");
  Context context(compiled.program);
  ASSERT_TRUE(context.ok());
  EXPECT_EQ(gltang_execution_attach_types(context.execution, a), GLTANG_OK);
  EXPECT_EQ(gltang_execution_attach_types(context.execution, other), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_execution_attach_types(context.execution, same), GLTANG_OK) << "the same declaration twice is two types, not a clash";
  gltang_type_registry_release(a);
  gltang_type_registry_release(same);
  gltang_type_registry_release(other);
}

// ---------------------------------------------------------------------------
// Instances
// ---------------------------------------------------------------------------

TEST(Custom, AnInstanceIsOfTheCustomKindAndOfItsType) {
  Types types;
  GLTANG_ValueKind seen = GLTANG_KIND_NULL;
  const GLTANG_Type * type = nullptr;
  Out o = run(PRELUDE "m = money(5); use probe; probe(m); m;", types, Config(), [&](Context & c, Types &) {
    static std::function<bool(GLTANG_NativeCall *)> body;
    body = [&](GLTANG_NativeCall * call) {
      GLTANG_Handle h;
      EXPECT_EQ(gltang_call_arg(call, 0, &h), GLTANG_OK);
      seen = gltang_handle_kind(call, h);
      type = gltang_handle_custom_type(call, h);
      gltang_call_return_null(call);
      return true;
    };
    ASSERT_EQ(gltang_library_add_native(c.library(), "probe", [](GLTANG_NativeCall * call, void * u) {
      return (*static_cast<std::function<bool(GLTANG_NativeCall *)> *>(u))(call);
    }, &body), GLTANG_OK);
  });
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(seen, GLTANG_KIND_CUSTOM);
  EXPECT_TRUE(gltang_type_same(type, types.money));
  EXPECT_EQ(o.kind, GLTANG_KIND_CUSTOM);
}

namespace {

struct ForeignMaker {
  const GLTANG_Type * type = nullptr;
  GLTANG_Result made = GLTANG_ERR_INTERNAL;
};

bool make_foreign(GLTANG_NativeCall * call, void * user) {
  ForeignMaker * f = static_cast<ForeignMaker *>(user);
  GLTANG_Handle h;
  f->made = gltang_handle_make_custom(call, f->type, &h);
  if (f->made == GLTANG_OK) {
    gltang_call_return_handle(call, h);
  }
  return true;
}

}  // namespace

TEST(Custom, AnInstanceOfATypeWhoseRegistryIsNotAttachedIsRefused) {
  Types types;
  Types elsewhere;  // never attached
  ForeignMaker f;
  f.type = elsewhere.money;
  Out o = run(PRELUDE "use foreign; foreign();", types, Config(), [&](Context & c, Types &) {
    ASSERT_EQ(gltang_library_add_native(c.library(), "foreign", make_foreign, &f), GLTANG_OK);
  });
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(f.made, GLTANG_ERR_INVALID);
  EXPECT_EQ(o.kind, GLTANG_KIND_NULL);
  // The control: a type of the attached registry is made.
  f.type = types.money;
  o = run(PRELUDE "use foreign; foreign();", types, Config(), [&](Context & c, Types &) {
    ASSERT_EQ(gltang_library_add_native(c.library(), "foreign", make_foreign, &f), GLTANG_OK);
  });
  EXPECT_EQ(f.made, GLTANG_OK);
  EXPECT_EQ(o.kind, GLTANG_KIND_CUSTOM);
  // NULL is refused too.
  f.type = nullptr;
  o = run(PRELUDE "use foreign; foreign();", types, Config(), [&](Context & c, Types &) {
    ASSERT_EQ(gltang_library_add_native(c.library(), "foreign", make_foreign, &f), GLTANG_OK);
  });
  EXPECT_EQ(f.made, GLTANG_ERR_INVALID);
}

namespace {

struct Bounds {
  const GLTANG_Type * type = nullptr;
  std::vector<GLTANG_Result> results;
  int64_t read_back = 0;
  GLTANG_ValueKind fresh_slot = GLTANG_KIND_BOOL;
};

bool probe_bounds(GLTANG_NativeCall * call, void * user) {
  Bounds * b = static_cast<Bounds *>(user);
  GLTANG_Handle m, v, s;
  EXPECT_EQ(gltang_handle_make_custom(call, b->type, &m), GLTANG_OK);
  EXPECT_EQ(gltang_handle_make_integer(call, 4, &v), GLTANG_OK);
  int64_t amount = 77;
  unsigned char buffer[16] = {0};
  b->results.push_back(gltang_handle_custom_slot_get(call, m, 0, &s));   // in range: OK
  b->fresh_slot = gltang_handle_kind(call, s);
  b->results.push_back(gltang_handle_custom_slot_get(call, m, 1, &s));   // past the one slot
  b->results.push_back(gltang_handle_custom_slot_set(call, m, 1, v));
  b->results.push_back(gltang_handle_custom_slot_set(call, m, 0, v));    // OK
  b->results.push_back(gltang_handle_custom_write_bytes(call, m, 0, &amount, sizeof amount));  // OK
  b->results.push_back(gltang_handle_custom_write_bytes(call, m, 1, &amount, sizeof amount));  // one past
  b->results.push_back(gltang_handle_custom_read_bytes(call, m, 0, buffer, 9));                 // one past
  b->results.push_back(gltang_handle_custom_read_bytes(call, m, SIZE_MAX, buffer, 2));          // wraps
  b->results.push_back(gltang_handle_custom_read_bytes(call, m, 8, buffer, 0));                 // the end, empty: OK
  b->results.push_back(gltang_handle_custom_read_bytes(call, m, 0, nullptr, 1));
  b->results.push_back(gltang_handle_custom_read_bytes(call, v, 0, buffer, 1));                 // not custom
  b->results.push_back(gltang_handle_custom_slot_get(call, v, 0, &s));                          // not custom
  EXPECT_EQ(gltang_handle_custom_read_bytes(call, m, 0, &b->read_back, sizeof b->read_back), GLTANG_OK);
  EXPECT_EQ(gltang_handle_custom_type(call, v), nullptr);
  gltang_call_return_handle(call, m);
  return true;
}

}  // namespace

TEST(Custom, SlotsAndBytesAreBoundsChecked) {
  Types types;
  Bounds b;
  b.type = types.money;
  Out o = run(PRELUDE "use bounds; bounds().label;", types, Config(), [&](Context & c, Types &) {
    ASSERT_EQ(gltang_library_add_native(c.library(), "bounds", probe_bounds, &b), GLTANG_OK);
  });
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(b.fresh_slot, GLTANG_KIND_NULL) << "a new instance's slots are null";
  std::vector<GLTANG_Result> want = {GLTANG_OK, GLTANG_ERR_INVALID, GLTANG_ERR_INVALID, GLTANG_OK, GLTANG_OK,
                                     GLTANG_ERR_INVALID, GLTANG_ERR_INVALID, GLTANG_ERR_INVALID, GLTANG_OK,
                                     GLTANG_ERR_INVALID, GLTANG_ERR_INVALID, GLTANG_ERR_INVALID};
  EXPECT_EQ(b.results, want);
  EXPECT_EQ(b.read_back, 77) << "a refused write wrote nothing";
  EXPECT_EQ(o.integer, 4) << "the slot holds what was stored";
}

// ---------------------------------------------------------------------------
// Binary operators and the reflection rule
// ---------------------------------------------------------------------------

TEST(Custom, TheLeftOperandsOperationAnswersWithSelfIsLhs) {
  Types types;
  Out o = run(PRELUDE "(money(2) + money(3)).amount;", types);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.integer, 5);
  EXPECT_EQ(o.log, (Log{"money add lhs=1 assign=0", "money get"})) << "the left answered, so the right was never asked";
}

TEST(Custom, ABuiltInLeftOperandReflectsToTheCustomRightOperand) {
  Types types;
  Out o = run(PRELUDE "(3 + money(5)).amount;", types);
  EXPECT_EQ(o.integer, 8);
  EXPECT_EQ(o.log, (Log{"money add lhs=0 assign=0", "money get"}));
  o = run(PRELUDE "(money(5) + 3).amount;", types);
  EXPECT_EQ(o.integer, 8);
  EXPECT_EQ(o.log, (Log{"money add lhs=1 assign=0", "money get"}));
  // The reflected call computes `other OP self`, with the operands as written.
  o = run(PRELUDE "(10 - money(3)).amount;", types);
  EXPECT_EQ(o.integer, 7);
  EXPECT_EQ(o.log, (Log{"money sub lhs=0 assign=0", "money get"}));
  o = run(PRELUDE "(money(10) - 3).amount;", types);
  EXPECT_EQ(o.integer, 7);
  o = run(PRELUDE "(20 / money(4)).amount;", types);
  EXPECT_EQ(o.integer, 5);
  o = run(PRELUDE "(20 % money(6)).amount;", types);
  EXPECT_EQ(o.integer, 2);
  o = run(PRELUDE "(2 * money(6)).amount;", types);
  EXPECT_EQ(o.integer, 12);
  EXPECT_EQ(o.log, (Log{"money mul lhs=0 assign=0", "money get"}));
}

TEST(Custom, WhenTheLeftDeclinesTheRightIsAskedWithTheOperandsSwapped) {
  Types types;
  Out o = run(PRELUDE "e = echo(); r = shy() + e; r == e;", types);
  ASSERT_TRUE(o.finished);
  EXPECT_TRUE(o.boolean) << "echo's add answered its own self";
  EXPECT_EQ(o.log, (Log{"shy binary lhs=1", "echo add lhs=0"}));
}

TEST(Custom, WhenBothDeclineTheValueIsNotSupported) {
  Types types;
  Out o = run(PRELUDE "shy() + shy();", types);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.error, GLTANG_ERROR_NOT_SUPPORTED);
  EXPECT_EQ(o.log, (Log{"shy binary lhs=1", "shy binary lhs=0"}));
  o = run(PRELUDE "plain() * plain();", types);
  EXPECT_EQ(o.error, GLTANG_ERROR_NOT_SUPPORTED);
  EXPECT_TRUE(o.log.empty()) << "a type with no entry is not called";
  o = run(PRELUDE "shy() - 1;", types);
  EXPECT_EQ(o.error, GLTANG_ERROR_NOT_SUPPORTED);
  EXPECT_EQ(o.log, (Log{"shy binary lhs=1"}));
  o = run(PRELUDE "money(1) + [1];", types);
  EXPECT_EQ(o.error, GLTANG_ERROR_NOT_SUPPORTED);
  o = run(PRELUDE "money(1) / 0;", types);
  EXPECT_EQ(o.error, GLTANG_ERROR_DIVIDE_BY_ZERO) << "an error the operation made is the answer";
}

TEST(Custom, AnErrorOperandIsTheValueAndTheTypeIsNotAsked) {
  Types types;
  // As a string's `+` gives its error operand: the error is not masked by a
  // decline, and no operation of the type runs.
  Out o = run(PRELUDE "money(1) + (1 / 0);", types);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.error, GLTANG_ERROR_DIVIDE_BY_ZERO);
  EXPECT_TRUE(o.log.empty()) << "the type was asked about an error operand";
  o = run(PRELUDE "(1 / 0) - money(1);", types);
  EXPECT_EQ(o.error, GLTANG_ERROR_DIVIDE_BY_ZERO) << "the left side's error, for the reflected call";
  EXPECT_TRUE(o.log.empty());
  o = run(PRELUDE "shy() < (1 / 0);", types);
  EXPECT_EQ(o.error, GLTANG_ERROR_DIVIDE_BY_ZERO) << "a comparison too";
  EXPECT_TRUE(o.log.empty()) << "shy would have declined, and the value would have been Not supported";
  o = run(PRELUDE "m = money(1); m += (1 / 0); m;", types);
  EXPECT_EQ(o.error, GLTANG_ERROR_DIVIDE_BY_ZERO) << "a compound assignment assigns the error";
  EXPECT_TRUE(o.log.empty());
  // Equality keeps its rule: the type is asked of any pair, an error included.
  o = run(PRELUDE "money(1) == (1 / 0);", types);
  EXPECT_EQ(o.kind, GLTANG_KIND_BOOL);
  EXPECT_FALSE(o.boolean);
  EXPECT_FALSE(o.log.empty()) << "equality asks the type";
}

TEST(Custom, ADeclineAllocatesNothingAndTheErrorIsMadeOnlyAfterBothDeclined) {
  Types types;
  // Warmed first: the first operation of an execution makes its handle frames
  // and its activation records. Then each measured operation is bracketed by
  // the heap's object count and the context allocator's call count.
  // Compiled code and the collector's instruments are kept out: an exit from
  // compiled code, and a collection forced at every allocation, are work of
  // their own, and what is counted here is the operation's.
  Config config;
  config.jit_threshold = 0;
  config.torture = 0;
  config.verify = 0;
  config.moving_stack = 0;
  config.relocate = 0;
  Out o = run(PRELUDE
              "allocs = t.allocs; mallocs = t.mallocs; s = shy(); e = echo(); w = s + e; w = s + s; w = s < e;"
              "a0 = allocs(); m0 = mallocs(); r = s + e; a1 = allocs(); m1 = mallocs();"
              "q = s + s; a2 = allocs();"
              "x = s < e; a3 = allocs();"
              "[a1 - a0, m1 - m0, a2 - a1, r == e, a3 - a2];",
              types, config);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.described, "[0, 0, 1, true, 1]")
      << "decline then answer: no object and no allocation at all; both decline: the one error object";
}

TEST(Custom, ADeclineNeverReachesTheErrorHooks) {
  Types types;
  // With every error logged at creation, a decline that the other side
  // answered leaves the list empty; with halt-on-error, it does not halt.
  Out o = run(PRELUDE "s = shy(); e = echo(); r = s + e; 7;", types, Config(), [](Context & c, Types &) {
    ASSERT_EQ(gltang_execution_set_log_all_errors(c.execution, true), GLTANG_OK);
    ASSERT_EQ(gltang_execution_set_halt_on_error(c.execution, true), GLTANG_OK);
  });
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.integer, 7);
  EXPECT_EQ(o.errors_listed, 0u);
  o = run(PRELUDE "s = shy(); r = s + s; 7;", types, Config(), [](Context & c, Types &) {
    ASSERT_EQ(gltang_execution_set_log_all_errors(c.execution, true), GLTANG_OK);
  });
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.errors_listed, 1u) << "the one error both declines make";
}

TEST(Custom, ACompoundAssignmentTellsTheOperation) {
  Types types;
  Out o = run(PRELUDE "m = money(1); m += 2; m.amount;", types);
  EXPECT_EQ(o.integer, 3);
  EXPECT_EQ(o.log, (Log{"money add lhs=1 assign=1", "money get"}));
  o = run(PRELUDE "m = money(1); m = m + 2; m.amount;", types);
  EXPECT_EQ(o.log, (Log{"money add lhs=1 assign=0", "money get"})) << "written out, it is not a compound assignment";
  o = run(PRELUDE "n = 5; n -= money(1); n.amount;", types);
  EXPECT_EQ(o.integer, 4);
  EXPECT_EQ(o.log, (Log{"money sub lhs=0 assign=1", "money get"}));
  o = run(PRELUDE "m = money(3); m *= 2; m /= 3; m %= 5; m.amount;", types);
  EXPECT_EQ(o.integer, 2);
  EXPECT_EQ(o.log, (Log{"money mul lhs=1 assign=1", "money div lhs=1 assign=1", "money mod lhs=1 assign=1", "money get"}));
}

TEST(Custom, ComparisonsReachTheOperationWithReflection) {
  Types types;
  struct Case {
    const char * source;
    bool answer;
    const char * log;
  } cases[] = {
    {"money(1) < money(2);", true, "money lt lhs=1"},
    {"money(1) <= 3;", true, "money le lhs=1"},
    {"3 < money(5);", true, "money lt lhs=0"},
    {"5 < money(3);", false, "money lt lhs=0"},
    {"m = money(4); m > m;", false, "money gt lhs=1"},
    {"money(3) >= money(2);", true, "money ge lhs=1"},
    {"2 >= money(3);", false, "money ge lhs=0"},
  };
  for (const Case & c : cases) {
    Out o = run(std::string(PRELUDE) + c.source, types);
    ASSERT_TRUE(o.finished) << c.source;
    EXPECT_EQ(o.kind, GLTANG_KIND_BOOL) << c.source << " gave " << o.described;
    EXPECT_EQ(o.boolean, c.answer) << c.source;
    EXPECT_EQ(o.log, (Log{c.log})) << c.source;
  }
  Out o = run(PRELUDE "plain() < plain();", types);
  EXPECT_EQ(o.error, GLTANG_ERROR_NOT_SUPPORTED);
  o = run(PRELUDE "shy() >= shy();", types);
  EXPECT_EQ(o.error, GLTANG_ERROR_NOT_SUPPORTED);
  EXPECT_EQ(o.log, (Log{"shy compare lhs=1", "shy compare lhs=0"}));
  o = run(PRELUDE "\"a\" < plain();", types);
  EXPECT_EQ(o.error, GLTANG_ERROR_NOT_SUPPORTED);
}

TEST(Custom, EqualityIsIdentityUnlessTheTypeSaysOtherwise) {
  Types types;
  struct Case {
    const char * source;
    bool answer;
  } cases[] = {
    {"p = plain(); p == p;", true},
    {"plain() == plain();", false},
    {"p = plain(); q = p; p == q;", true},
    {"plain() == \"x\";", false},
    {"\"x\" == plain();", false},
    {"plain() != null;", true},
    {"null == plain();", false},
    {"plain() == 3;", false},
    {"plain() == [];", false},
    {"p = plain(); p != p;", false},
    {"money(2) == money(2);", true},
    {"money(2) == money(3);", false},
    {"3 == money(3);", true},
    {"money(2) != money(3);", true},
    {"money(2) != 2;", false},
    {"p = plain(); [p] == [p];", true},
    {"[plain()] == [plain()];", false},
    {"[money(1), 2] == [money(1), 2];", true},
    {"[money(1)] == [money(2)];", false},
    {"s = shy(); s == s;", true},
    {"shy() == shy();", false},
  };
  for (const Case & c : cases) {
    Out o = run(std::string(PRELUDE) + c.source, types);
    ASSERT_TRUE(o.finished) << c.source;
    EXPECT_EQ(o.kind, GLTANG_KIND_BOOL) << c.source << " gave " << o.described;
    EXPECT_EQ(o.boolean, c.answer) << c.source;
  }
  // The operation is consulted, with reflection, before the language's rule.
  Out o = run(PRELUDE "3 == money(3);", types);
  EXPECT_EQ(o.log, (Log{"money eq lhs=0"}));
  o = run(PRELUDE "money(2) != money(3);", types);
  EXPECT_EQ(o.log, (Log{"money eq lhs=1"})) << "money has no not_equal: != asks equal and answers its opposite";
  o = run(PRELUDE "s = shy(); s != s;", types);
  EXPECT_EQ(o.log, (Log{"shy compare lhs=1", "shy compare lhs=0", "shy compare lhs=1", "shy compare lhs=0"}))
      << "not_equal both ways, then equal both ways, then identity";
  EXPECT_FALSE(o.boolean);
}

TEST(Custom, ABuiltInLeftOperandHasNoRuleForACustomOne) {
  Types types;
  Out o = run(PRELUDE "\"xyz\" + money(1);", types);
  EXPECT_EQ(o.integer, 4) << "the string rule declined, so money's own rule for a string answered";
  EXPECT_EQ(o.log, (Log{"money add lhs=0 assign=0"}));
  o = run(PRELUDE "money(1) + \"xyz\";", types);
  EXPECT_EQ(o.integer, 4);
  EXPECT_EQ(o.log, (Log{"money add lhs=1 assign=0"}));
  o = run(PRELUDE "\"x\" + plain();", types);
  EXPECT_EQ(o.error, GLTANG_ERROR_NOT_SUPPORTED) << "never \"x\" and an empty string";
  o = run(PRELUDE "plain() + \"x\";", types);
  EXPECT_EQ(o.error, GLTANG_ERROR_NOT_SUPPORTED);
  o = run(PRELUDE "[1] + money(1);", types);
  EXPECT_EQ(o.error, GLTANG_ERROR_NOT_SUPPORTED);
  EXPECT_EQ(o.log, (Log{"money add lhs=0 assign=0"}));
  o = run(PRELUDE "[1] * money(2);", types);
  EXPECT_EQ(o.error, GLTANG_ERROR_NOT_SUPPORTED);
  o = run(PRELUDE "(1.5 + money(2)).amount;", types);
  EXPECT_EQ(o.error, GLTANG_ERROR_NOT_IMPLEMENTED) << "money declines a float; the error has no attribute";
}

// ---------------------------------------------------------------------------
// The operations on one custom operand
// ---------------------------------------------------------------------------

TEST(Custom, NegateAttributeIndexAndSliceReachTheType) {
  Types types;
  Out o = run(PRELUDE "(-money(4)).amount;", types);
  EXPECT_EQ(o.integer, -4);
  o = run(PRELUDE "money(7).amount;", types);
  EXPECT_EQ(o.integer, 7);
  EXPECT_EQ(o.log, (Log{"money get"}));
  o = run(PRELUDE "m = money(1); m.amount = 5; m.amount;", types);
  EXPECT_EQ(o.integer, 5);
  EXPECT_EQ(o.log, (Log{"money set", "money get"}));
  o = run(PRELUDE "m = money(1); m.label = [1, 2]; m.label;", types);
  EXPECT_EQ(o.described, "[1, 2]");
  o = run(PRELUDE "m = money(1); m.amount = 9;", types);
  EXPECT_EQ(o.integer, 9) << "an assignment with no answer of its own is worth the value assigned";
  o = run(PRELUDE "m = money(1); m[0] = 5;", types);
  EXPECT_EQ(o.integer, 5) << "so is an index assignment";
  o = run(PRELUDE "money(1)[2];", types);
  EXPECT_EQ(o.integer, 2);
  EXPECT_EQ(o.log, (Log{"money index"}));
  o = run(PRELUDE "money(1)[\"k\"];", types);
  EXPECT_EQ(o.described, "k") << "any value is an index";
  o = run(PRELUDE "m = money(1); m[\"k\"] = \"v\"; m.label;", types);
  EXPECT_EQ(o.described, "v");
  EXPECT_EQ(o.log, (Log{"money set_index", "money get"}));
  o = run(PRELUDE "money(1)[1:3];", types);
  EXPECT_EQ(o.described, "[1, 3, null]");
  o = run(PRELUDE "money(1)[::2];", types);
  EXPECT_EQ(o.described, "[null, null, 2]");
  o = run(PRELUDE "money(1)[:];", types);
  EXPECT_EQ(o.described, "[null, null, null]");
  EXPECT_EQ(o.log, (Log{"money slice"}));
}

TEST(Custom, AnOperationATypeLacksOrDeclinesIsTheLanguagesError) {
  Types types;
  struct Case {
    const char * source;
    GLTANG_ErrorKind error;
  } cases[] = {
    {"-plain();", GLTANG_ERROR_NOT_SUPPORTED},
    {"-shy();", GLTANG_ERROR_NOT_SUPPORTED},
    {"plain().x;", GLTANG_ERROR_NOT_IMPLEMENTED},
    {"shy().x;", GLTANG_ERROR_NOT_IMPLEMENTED},
    {"money(1).missing;", GLTANG_ERROR_NOT_IMPLEMENTED},
    {"p = plain(); p.x = 1;", GLTANG_ERROR_NOT_SUPPORTED},
    {"s = shy(); s.x = 1;", GLTANG_ERROR_NOT_SUPPORTED},
    {"m = money(1); m.missing = 1;", GLTANG_ERROR_NOT_SUPPORTED},
    {"plain()[0];", GLTANG_ERROR_NOT_SUPPORTED},
    {"shy()[0];", GLTANG_ERROR_NOT_SUPPORTED},
    {"p = plain(); p[0] = 1;", GLTANG_ERROR_NOT_SUPPORTED},
    {"s = shy(); s[0] = 1;", GLTANG_ERROR_NOT_SUPPORTED},
    {"plain()[1:2];", GLTANG_ERROR_NOT_SUPPORTED},
    {"shy()[1:];", GLTANG_ERROR_NOT_SUPPORTED},
    {"plain() as int;", GLTANG_ERROR_NOT_SUPPORTED},
    {"plain() as string;", GLTANG_ERROR_NOT_SUPPORTED},
    {"plain() as float;", GLTANG_ERROR_NOT_SUPPORTED},
    {"plain() as bool;", GLTANG_ERROR_NOT_SUPPORTED},
    {"plain()();", GLTANG_ERROR_INVALID_FUNCTION_CALL},
    {"x = 0; for (i : plain()) { x = 1; }", GLTANG_ERROR_NOT_SUPPORTED},
  };
  for (const Case & c : cases) {
    Out o = run(std::string(PRELUDE) + c.source, types);
    ASSERT_TRUE(o.finished) << c.source;
    EXPECT_EQ(o.kind, GLTANG_KIND_ERROR) << c.source << " gave " << o.described;
    EXPECT_EQ(o.error, c.error) << c.source << " gave " << o.described;
  }
}

TEST(Custom, TheOperationsOfLaterStoriesBehaveAsForAFunction) {
  Types types;
  Out o = run(PRELUDE "print(\"a\"); print(plain()); print(\"b\");", types);
  EXPECT_EQ(o.raw, "ab") << "print appends nothing";
  o = run(PRELUDE "plain() ? 1 : 2;", types);
  EXPECT_EQ(o.integer, 1) << "truthy";
  o = run(PRELUDE "!plain();", types);
  EXPECT_FALSE(o.boolean);
  o = run(PRELUDE "[plain()];", types);
  EXPECT_EQ(o.described, "[Custom: plain]") << "an element shows its type's name";
}

// ---------------------------------------------------------------------------
// Instances held, cycles, and the collector
// ---------------------------------------------------------------------------

TEST(Custom, AnInstanceInAContainerIsTheSameObjectAndAContainerInASlotIsACopy) {
  Types types;
  Out o = run(PRELUDE "p = plain(); a = [p, p]; m = {k: p}; [a[0] == a[1], m.k == p, a[0] == plain()];", types);
  EXPECT_EQ(o.described, "[true, true, false]");
  o = run(PRELUDE "a = [1]; p = plain(); t.setslot(p, 0, a); a[0] = 2; [t.slot(p, 0)[0], a[0]];", types);
  EXPECT_EQ(o.described, "[1, 2]") << "the slot holds a copy, as every store of a container does";
  o = run(PRELUDE "q = plain(); p = plain(); t.setslot(p, 1, q); t.slot(p, 1) == q;", types);
  EXPECT_TRUE(o.boolean) << "an instance in a slot of another is that instance";
}

TEST(Custom, ACycleThroughSlotsIsCollected) {
  Types types;
  Out o = run(PRELUDE
              "a = plain(); b = plain(); t.setslot(a, 0, b); t.setslot(b, 0, a); t.track(a); t.track(b);"
              "a = null; b = null; t.alive();",
              types);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.integer, 0) << "neither is reachable: the cycle is garbage";
  // The control: the same cycle, still held, survives.
  o = run(PRELUDE "a = plain(); b = plain(); t.setslot(a, 0, b); t.setslot(b, 0, a); t.track(a); t.track(b); b = null; t.alive();",
          types);
  EXPECT_EQ(o.integer, 2) << "b is reached through a's slot";
}

TEST(Custom, WhatASlotHoldsSurvivesACollectionAtEveryAllocation) {
  Types types;
  // The only reference to the array of maps is the slot; under torture every
  // allocation below collects, and verify checks every store.
  const char * source = PRELUDE
      "p = plain(); m = money(3); t.setslot(p, 0, [{k: \"v\", n: [1, 2, 3]}, \"s\" + \"t\"]); t.setslot(p, 1, m);"
      "m = null; i = 0; while (i < 50) { x = [i, {j: i}]; i += 1; }"
      "s = t.slot(p, 0); [s[0].k, s[0].n[2], s[1], t.slot(p, 1).amount];";
  Out o = run(source, types, forced_torture());
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.described, "[v, 3, st, 3]");
  // And under the instruments the environment chose (the relocation arm, a moving stack).
  o = run(source, types);
  EXPECT_EQ(o.described, "[v, 3, st, 3]");
}

TEST(Custom, OperationsUnderTortureAndVerifyKeepEveryOperandAlive) {
  Types types;
  Out o = run(PRELUDE
              "m = money(1); i = 0; while (i < 20) { m += i; m = 1 + m; i += 1; }"
              "[m.amount, (money(2) < m), m[[1, 2]], money(1)[1:2:3], -m == money(0) - m];",
              types, forced_torture());
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.described, "[211, true, [1, 2], [1, 2, 3], true]");
}

// ---------------------------------------------------------------------------
// Charging, budgets and the host-function contract
// ---------------------------------------------------------------------------

namespace {

uint64_t fuel_of(const std::string & source, Types & types) {
  uint64_t fuel = 0;
  Compiled compiled(source);
  Context context(compiled.program);
  EXPECT_TRUE(context.ok());
  types.context = &context;
  GLTANG_Library * t = nullptr;
  EXPECT_EQ(gltang_library_create("t", &t), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_native(t, "money", make_money, &types), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_native(t, "plain", make_plain_like<&Types::plain>, &types), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_native(t, "shy", make_plain_like<&Types::shy>, &types), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_native(t, "echo", make_plain_like<&Types::echo>, &types), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_library(context.library(), t), GLTANG_OK);
  gltang_library_release(t);
  EXPECT_EQ(gltang_execution_attach_types(context.execution, types.registry), GLTANG_OK);
  EXPECT_TRUE(context.execute());
  fuel = grcore_context_fuel_used(context.context);
  types.context = nullptr;
  return fuel;
}

GLTANG_OpStatus talker_add(GLTANG_NativeCall * call, void *, GLTANG_Handle, GLTANG_Handle, bool, bool) {
  std::string text(640, 'w');
  GLTANG_Handle h;
  if (gltang_handle_make_string(call, text.data(), text.size(), GLTANG_UNICODE_STRING_TYPE_TRUSTED, &h) != GLTANG_OK) {
    return GLTANG_OP_ERROR;
  }
  gltang_call_return_handle(call, h);
  return GLTANG_OP_OK;
}

}  // namespace

TEST(Custom, AnOperationIsChargedAsANativeCallPlusTheBytesItAnswers) {
  Types types;
  // The same program with an operation that calls the type and one that does
  // not: e = echo() answers itself, so the difference is the callback alone.
  uint64_t with = fuel_of(PRELUDE "e = echo(); r = e + 1;", types);
  uint64_t without = fuel_of(PRELUDE "e = echo(); r = 2 + 1;", types);
  EXPECT_EQ(with, without + 1) << "one native call";
  uint64_t both = fuel_of(PRELUDE "s = shy(); e = echo(); r = s + e;", types);
  uint64_t none = fuel_of(PRELUDE "s = shy(); e = echo(); r = 2 + 1;", types);
  EXPECT_EQ(both, none + 2) << "a reflected operation is two calls";
  // A type whose add answers a 640-byte string: one call and ten units of bytes.
  GLTANG_TypeRegistry * r = nullptr;
  ASSERT_EQ(gltang_type_registry_create(&r), GLTANG_OK);
  GLTANG_TypeDescriptor d = GLTANG_TYPE_DESCRIPTOR_INIT;
  d.name = "talker";
  d.add = talker_add;
  d.thread_safe = true;  // two contexts, one after the other
  const GLTANG_Type * talker = nullptr;
  ASSERT_EQ(gltang_type_registry_add(r, &d, &talker), GLTANG_OK);
  auto fuel = [&](const char * source) {
    Compiled compiled(source);
    Context context(compiled.program);
    EXPECT_TRUE(context.ok());
    ForeignMaker maker;
    maker.type = talker;
    EXPECT_EQ(gltang_library_add_native(context.library(), "talker", make_foreign, &maker), GLTANG_OK);
    EXPECT_EQ(gltang_execution_attach_types(context.execution, r), GLTANG_OK);
    EXPECT_TRUE(context.execute());
    return grcore_context_fuel_used(context.context);
  };
  // The call, the string's own making (its bytes and one, as every string a host
  // gives is charged), and the bytes the operation answers.
  EXPECT_EQ(fuel("use talker; x = talker(); r = x + 1;"),
            fuel("use talker; x = talker(); r = 2 + 1;") + 1u + (640u / GLTANG_WORK_BYTES_PER_FUEL + 1u) + 640u / GLTANG_WORK_BYTES_PER_FUEL);
  gltang_type_registry_release(r);
}

namespace {

struct Hog {
  int calls = 0;
};

GLTANG_OpStatus hog_add(GLTANG_NativeCall * call, void * user, GLTANG_Handle, GLTANG_Handle, bool, bool) {
  ++static_cast<Hog *>(user)->calls;
  // Makes arrays it keeps (each has a handle) until the context's memory says
  // no; then answers anyway, and the answer is not what the call is worth.
  GLTANG_Handle h;
  for (int i = 0; i < 1000; ++i) {
    if (gltang_handle_make_array(call, 4096, &h) != GLTANG_OK) {
      break;
    }
  }
  gltang_call_return_integer(call, 1);
  return GLTANG_OP_OK;
}

GLTANG_OpStatus poller_add(GLTANG_NativeCall * call, void *, GLTANG_Handle, GLTANG_Handle, bool, bool) {
  for (int i = 0; i < 1000000; ++i) {
    if (gltang_call_poll(call, 100) != GLTANG_OK) {
      // The run is to stop: whatever is answered, it unwinds.
      return GLTANG_OP_DECLINE;
    }
  }
  return GLTANG_OP_OK;
}

}  // namespace

TEST(Custom, AnOperationThatRunsOutOfMemoryIsTheCallsOutOfMemoryAndTheRunGoesOn) {
  Hog hog;
  GLTANG_TypeRegistry * r = nullptr;
  ASSERT_EQ(gltang_type_registry_create(&r), GLTANG_OK);
  GLTANG_TypeDescriptor d = GLTANG_TYPE_DESCRIPTOR_INIT;
  d.name = "hog";
  d.user = &hog;
  d.add = hog_add;
  const GLTANG_Type * type = nullptr;
  ASSERT_EQ(gltang_type_registry_add(r, &d, &type), GLTANG_OK);
  Config config;
  config.memory_bytes = 8u << 20;
  Compiled compiled("use hog; h = hog(); r = h + 1; [r, 7];");
  Context context(compiled.program, config);
  ASSERT_TRUE(context.ok());
  ForeignMaker maker;
  maker.type = type;
  ASSERT_EQ(gltang_library_add_native(context.library(), "hog", make_foreign, &maker), GLTANG_OK);
  ASSERT_EQ(gltang_execution_attach_types(context.execution, r), GLTANG_OK);
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(hog.calls, 1);
  EXPECT_EQ(context.describe(), "[Error: Out of memory, 7]");
  gltang_type_registry_release(r);
}

TEST(Custom, AnOperationThatPollsIsStoppedByTheBudgetAsANativeIs) {
  GLTANG_TypeRegistry * r = nullptr;
  ASSERT_EQ(gltang_type_registry_create(&r), GLTANG_OK);
  GLTANG_TypeDescriptor d = GLTANG_TYPE_DESCRIPTOR_INIT;
  d.name = "poller";
  d.add = poller_add;
  const GLTANG_Type * type = nullptr;
  ASSERT_EQ(gltang_type_registry_add(r, &d, &type), GLTANG_OK);
  Config config;
  config.fuel = 100000;
  Compiled compiled("use poller; p = poller(); r = p + 1; 7;");
  Context context(compiled.program, config);
  ASSERT_TRUE(context.ok());
  ForeignMaker maker;
  maker.type = type;
  ASSERT_EQ(gltang_library_add_native(context.library(), "poller", make_foreign, &maker), GLTANG_OK);
  ASSERT_EQ(gltang_execution_attach_types(context.execution, r), GLTANG_OK);
  EXPECT_FALSE(context.execute()) << "the budget ran out inside the operation";
  EXPECT_FALSE(context.is_integer()) << "the run did not go on to its last statement";
  uint64_t polls = 0;
  EXPECT_EQ(gltang_execution_native_polls(context.execution, "HOST_CALLBACK", &polls), GLTANG_OK);
  EXPECT_GT(polls, 0u);
  EXPECT_LE(grcore_context_fuel_used(context.context), 100000u + 200u) << "stopped within one poll's work of the budget";
  gltang_type_registry_release(r);
}

namespace {

struct HostCheck {
  GLTANG_Execution * execution = nullptr;
  GLTANG_Result set = GLTANG_ERR_INTERNAL;
  uint64_t native_depth = 0;
  GRCORE_Context * context = nullptr;
};

GLTANG_OpStatus host_check_add(GLTANG_NativeCall * call, void * user, GLTANG_Handle self, GLTANG_Handle, bool, bool) {
  HostCheck * h = static_cast<HostCheck *>(user);
  // A host function: it cannot reconfigure or destroy the execution. (The
  // handle limits are the setter refused for that reason alone: the others are
  // refused once the run has started anyway.)
  h->set = gltang_execution_set_handle_limits(h->execution, 0, 0);
  if (h->set == GLTANG_ERR_INVALID) {
    // Only where the setter was refused: an execution destroyed under its
    // own run would end the test in a crash, not a failure.
    gltang_execution_destroy(h->execution);
  }
  h->native_depth = grcore_context_depth(h->context, GRCORE_DEPTH_NATIVE);
  gltang_call_return_handle(call, self);
  return GLTANG_OP_OK;
}

}  // namespace

TEST(Custom, AnOperationRunsAsAHostFunctionWithOneUnitOfNativeDepth) {
  HostCheck check;
  GLTANG_TypeRegistry * r = nullptr;
  ASSERT_EQ(gltang_type_registry_create(&r), GLTANG_OK);
  GLTANG_TypeDescriptor d = GLTANG_TYPE_DESCRIPTOR_INIT;
  d.name = "host_check";
  d.user = &check;
  d.add = host_check_add;
  const GLTANG_Type * type = nullptr;
  ASSERT_EQ(gltang_type_registry_add(r, &d, &type), GLTANG_OK);
  Compiled compiled("use mk; x = mk(); r = x + 1; 7;");
  Context context(compiled.program);
  ASSERT_TRUE(context.ok());
  check.execution = context.execution;
  check.context = context.context;
  ForeignMaker maker;
  maker.type = type;
  ASSERT_EQ(gltang_library_add_native(context.library(), "mk", make_foreign, &maker), GLTANG_OK);
  ASSERT_EQ(gltang_execution_attach_types(context.execution, r), GLTANG_OK);
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(context.integer(), 7) << "the execution survived the destroy from inside";
  EXPECT_EQ(check.set, GLTANG_ERR_INVALID);
  EXPECT_EQ(check.native_depth, 1u);
  gltang_type_registry_release(r);
}

TEST(Custom, ANativeDepthBudgetThatIsFullRefusesTheOperation) {
  Types types;
  Config config;
  config.native_depth = 1;
  // At native depth 1 a native can run (the constructor), and an operation
  // made from inside one could not; at the top level it can.
  Out o = run(PRELUDE "(money(1) + money(2)).amount;", types, config);
  EXPECT_EQ(o.integer, 3);
  // Inside a native (the engine's test native `reenter`, which runs f as a
  // nested activation) the budget is full: the operation's record is refused,
  // the callback never runs, and the value is the recursion-limit error.
  uint64_t depth_after = 99;
  o = run(PRELUDE "use reenter; function f(m) { return m + 1; } reenter(f, money(1));", types, config, [&](Context & c, Types &) {
    c.add_native_library();
  });
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.error, GLTANG_ERROR_RECURSION_LIMIT) << o.described;
  EXPECT_TRUE(o.log.empty()) << "the callback ran with its record refused";
  {
    Compiled compiled(PRELUDE "use reenter; function f(m) { return m + 1; } reenter(f, money(1));");
    Context context(compiled.program, config);
    ASSERT_TRUE(context.ok());
    types.context = &context;
    GLTANG_Library * lib = nullptr;
    ASSERT_EQ(gltang_library_create("t", &lib), GLTANG_OK);
    ASSERT_EQ(gltang_library_add_native(lib, "money", make_money, &types), GLTANG_OK);
    ASSERT_EQ(gltang_library_add_native(lib, "plain", make_plain_like<&Types::plain>, &types), GLTANG_OK);
    ASSERT_EQ(gltang_library_add_native(lib, "shy", make_plain_like<&Types::shy>, &types), GLTANG_OK);
    ASSERT_EQ(gltang_library_add_native(lib, "echo", make_plain_like<&Types::echo>, &types), GLTANG_OK);
    ASSERT_EQ(gltang_library_add_library(context.library(), lib), GLTANG_OK);
    gltang_library_release(lib);
    context.add_native_library();
    ASSERT_EQ(gltang_execution_attach_types(context.execution, types.registry), GLTANG_OK);
    ASSERT_TRUE(context.execute());
    depth_after = grcore_context_depth(context.context, GRCORE_DEPTH_NATIVE);
    types.context = nullptr;
  }
  EXPECT_EQ(depth_after, 0u) << "the refused record left the native depth where it was";
}

TEST(Custom, ACustomLeftOperandWithNoSuchOperationReflectsToTheRight) {
  Types types;
  Out o = run(PRELUDE "e = echo(); r = plain() + e; r == e;", types);
  ASSERT_TRUE(o.finished);
  EXPECT_TRUE(o.boolean) << "echo's add answered";
  EXPECT_EQ(o.log, (Log{"echo add lhs=0"}));
  o = run(PRELUDE "plain() < money(1);", types);
  EXPECT_EQ(o.error, GLTANG_ERROR_NOT_SUPPORTED);
  EXPECT_EQ(o.log, (Log{"money lt lhs=0"}));
}

TEST(TypeRegistry, AThreadUnsafeRegistryStaysBoundAfterItsContextIsDestroyed) {
  Types unsafe(false);
  Compiled compiled("1;");
  for (int k = 0; k < 4; ++k) {
    // One context at a time, each destroyed before the next is made (a later
    // one may well be given the address of an earlier one).
    Context context(compiled.program);
    ASSERT_TRUE(context.ok());
    EXPECT_EQ(gltang_execution_attach_types(context.execution, unsafe.registry), k == 0 ? GLTANG_OK : GLTANG_ERR_INVALID) << "context " << k;
  }
}

namespace {

GLTANG_OpStatus five_equal(GLTANG_NativeCall * call, void *, GLTANG_Handle, GLTANG_Handle, bool) {
  gltang_call_return_integer(call, 5);
  return GLTANG_OP_OK;
}

}  // namespace

TEST(Custom, AnEqualThatAnswersOtherThanTrueMakesNotEqualTrue) {
  GLTANG_TypeRegistry * r = nullptr;
  ASSERT_EQ(gltang_type_registry_create(&r), GLTANG_OK);
  GLTANG_TypeDescriptor d = GLTANG_TYPE_DESCRIPTOR_INIT;
  d.name = "five";
  d.equal = five_equal;
  const GLTANG_Type * type = nullptr;
  ASSERT_EQ(gltang_type_registry_add(r, &d, &type), GLTANG_OK);
  Compiled compiled("use mk; f = mk(); [f == f, f != f];");
  Context context(compiled.program);
  ASSERT_TRUE(context.ok());
  ForeignMaker maker;
  maker.type = type;
  ASSERT_EQ(gltang_library_add_native(context.library(), "mk", make_foreign, &maker), GLTANG_OK);
  ASSERT_EQ(gltang_execution_attach_types(context.execution, r), GLTANG_OK);
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(context.describe(), "[5, true]") << "`==` is equal's answer; `!=` is false only for an answer of exactly true";
  gltang_type_registry_release(r);
}

namespace {

GLTANG_OpStatus failing_equal(GLTANG_NativeCall * call, void *, GLTANG_Handle, GLTANG_Handle, bool) {
  gltang_call_return_error(call, GLTANG_ERROR_DIVIDE_BY_ZERO);
  return GLTANG_OP_ERROR;
}

}  // namespace

TEST(Custom, AnErrorAnElementsEqualAnswersIsTheContainerComparisonsValue) {
  GLTANG_TypeRegistry * r = nullptr;
  ASSERT_EQ(gltang_type_registry_create(&r), GLTANG_OK);
  GLTANG_TypeDescriptor d = GLTANG_TYPE_DESCRIPTOR_INIT;
  d.name = "failing";
  d.equal = failing_equal;
  const GLTANG_Type * type = nullptr;
  ASSERT_EQ(gltang_type_registry_add(r, &d, &type), GLTANG_OK);
  // At the top level and as an element, nested or not, `==` and `!=` are the
  // error; the error is falsy, so a test of the comparison takes the else arm.
  Compiled compiled(
      "use mk; f = mk(); g = mk();"
      "t = 0; if ([1, f] == [1, g]) { t = 1; } else { t = 2; }"
      "[f == g, [f] == [g], [1, [f]] != [1, [g]], [0, f] == [1, g], t];");
  Context context(compiled.program);
  ASSERT_TRUE(context.ok());
  ForeignMaker maker;
  maker.type = type;
  ASSERT_EQ(gltang_library_add_native(context.library(), "mk", make_foreign, &maker), GLTANG_OK);
  ASSERT_EQ(gltang_execution_attach_types(context.execution, r), GLTANG_OK);
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(context.describe(), "[Error: Divide by zero, Error: Divide by zero, Error: Divide by zero, false, 2]")
      << "an element's error is the comparison's value; elements before it that differ decide first";
  gltang_type_registry_release(r);
}

// ---------------------------------------------------------------------------
// Snapshots, compiled code, threads
// ---------------------------------------------------------------------------

TEST(Custom, ASnapshotOfAnExecutionHoldingAnInstanceIsRefusedNamingTheType) {
  for (bool hold : {true, false}) {
    Types types;
    Config config;
    config.fuel = 2000;
    std::string source = std::string(PRELUDE) + (hold ? "p = plain();" : "p = 1;") + " i = 0; while (true) { i += 1; }";
    Compiled compiled(source);
    Context context(compiled.program, config);
    ASSERT_TRUE(context.ok());
    types.context = &context;
    GLTANG_Library * t = nullptr;
    ASSERT_EQ(gltang_library_create("t", &t), GLTANG_OK);
    ASSERT_EQ(gltang_library_add_native(t, "money", make_money, &types), GLTANG_OK);
    ASSERT_EQ(gltang_library_add_native(t, "plain", make_plain_like<&Types::plain>, &types), GLTANG_OK);
    ASSERT_EQ(gltang_library_add_native(t, "shy", make_plain_like<&Types::shy>, &types), GLTANG_OK);
    ASSERT_EQ(gltang_library_add_native(t, "echo", make_plain_like<&Types::echo>, &types), GLTANG_OK);
    ASSERT_EQ(gltang_library_add_library(context.library(), t), GLTANG_OK);
    gltang_library_release(t);
    ASSERT_EQ(gltang_execution_attach_types(context.execution, types.registry), GLTANG_OK);
    context.execute();
    ASSERT_TRUE(context.paused());
    GLTANG_Snapshot * snapshot = nullptr;
    GLTANG_Result taken = gltang_snapshot_take(context.execution, &snapshot);
    if (hold) {
      EXPECT_EQ(taken, GLTANG_ERR_UNSUPPORTED);
      EXPECT_STREQ(gltang_type_name(gltang_snapshot_refused_type(context.execution)), "plain");
      EXPECT_EQ(snapshot, nullptr);
    }
    else {
      // The control: the same execution with no instance live is captured.
      EXPECT_EQ(taken, GLTANG_OK);
      EXPECT_EQ(gltang_snapshot_refused_type(context.execution), nullptr);
      gltang_snapshot_release(snapshot);
    }
    types.context = nullptr;
  }
}

namespace {

/// keep(x): promotes x to a persistent handle and leaves it held.
bool keep(GLTANG_NativeCall * call, void *) {
  GLTANG_Handle x;
  GLTANG_Persistent p;
  if (gltang_call_arg(call, 0, &x) != GLTANG_OK || gltang_handle_promote(call, x, &p) != GLTANG_OK) {
    return false;
  }
  gltang_call_return_null(call);
  return true;
}

}  // namespace

TEST(Custom, ATakeRefusedForAnotherReasonClearsTheTypeAnEarlierTakeNamed) {
  Types types;
  Config config;
  config.fuel = 2000;
  // Paused in the first loop with an instance live, then in the second with a
  // persistent handle held as well.
  Compiled compiled(PRELUDE "p = plain(); i = 0; while (i < 20000) { i += 1; } use keep; keep(1); while (true) { i += 1; }");
  Context context(compiled.program, config);
  ASSERT_TRUE(context.ok());
  types.context = &context;
  GLTANG_Library * lib = nullptr;
  ASSERT_EQ(gltang_library_create("t", &lib), GLTANG_OK);
  ASSERT_EQ(gltang_library_add_native(lib, "money", make_money, &types), GLTANG_OK);
  ASSERT_EQ(gltang_library_add_native(lib, "plain", make_plain_like<&Types::plain>, &types), GLTANG_OK);
  ASSERT_EQ(gltang_library_add_native(lib, "shy", make_plain_like<&Types::shy>, &types), GLTANG_OK);
  ASSERT_EQ(gltang_library_add_native(lib, "echo", make_plain_like<&Types::echo>, &types), GLTANG_OK);
  ASSERT_EQ(gltang_library_add_library(context.library(), lib), GLTANG_OK);
  gltang_library_release(lib);
  ASSERT_EQ(gltang_library_add_native(context.library(), "keep", keep, nullptr), GLTANG_OK);
  ASSERT_EQ(gltang_execution_attach_types(context.execution, types.registry), GLTANG_OK);
  context.execute();
  ASSERT_TRUE(context.paused());
  GLTANG_Snapshot * snapshot = nullptr;
  EXPECT_EQ(gltang_snapshot_take(context.execution, &snapshot), GLTANG_ERR_UNSUPPORTED);
  EXPECT_STREQ(gltang_type_name(gltang_snapshot_refused_type(context.execution)), "plain");
  ASSERT_FALSE(context.finished_after_raising(1000000));
  ASSERT_TRUE(context.paused());
  EXPECT_EQ(gltang_snapshot_take(context.execution, &snapshot), GLTANG_ERR_UNSUPPORTED) << "a persistent handle is held";
  EXPECT_EQ(gltang_snapshot_refused_type(context.execution), nullptr) << "this take was not refused for a type";
  EXPECT_EQ(snapshot, nullptr);
  types.context = nullptr;
}

TEST(Custom, CompiledCodeLeavesForTheInterpreterAndAnswersTheSame) {
  Types types;
  const char * source = PRELUDE
      "function f(m, k) { return m + k; } s = 0; i = 0;"
      "while (i < 40) { r = f(money(i), i); s += r.amount; i += 1; } s;";
  for (long threshold : {0L, 1L}) {
    Config config;
    config.jit_threshold = threshold;
    Out o = run(source, types, config);
    ASSERT_TRUE(o.finished);
    EXPECT_EQ(o.integer, 1560) << "threshold " << threshold;
  }
}

TEST(Custom, TwoContextsOnTwoThreadsShareOneThreadSafeRegistry) {
  Types types(true);
  // The program and the library are made here, once, and shared by both threads
  // (both are immutable once attached): cutil's allocation counters are plain
  // integers, so the threads must not allocate through it. Each context
  // allocates through its own tracker.
  Compiled compiled("use mk; a = mk(); b = mk(); a == a && !(a == b);");
  ASSERT_TRUE(compiled.ok());
  GLTANG_Library * library = nullptr;
  ASSERT_EQ(gltang_library_create(nullptr, &library), GLTANG_OK);
  ASSERT_EQ(gltang_library_add_native(library, "mk", make_plain_like<&Types::plain>, &types), GLTANG_OK);
  std::atomic<int> agreed{0};
  const int rounds = tt::heavy_instruments() ? 2 : 10;
  auto body = [&]() {
    for (int k = 0; k < rounds; ++k) {
      Context context(compiled.program);
      EXPECT_EQ(gltang_execution_set_libraries(context.execution, library), GLTANG_OK);
      EXPECT_EQ(gltang_execution_attach_types(context.execution, types.registry), GLTANG_OK);
      EXPECT_TRUE(context.execute());
      agreed += context.boolean() ? 1 : 0;
    }
  };
  std::thread one(body);
  std::thread two(body);
  one.join();
  two.join();
  gltang_library_release(library);
  EXPECT_EQ(agreed.load(), 2 * rounds);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
