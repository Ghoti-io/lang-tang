// A host callback that calls guest code (spec-tang-host-extensibility story 9; CAP-9, AD-23).
//
// A callback asks for a guest call with gltang_call_guest and returns GLTANG_OP_PENDING; the
// interpreter makes the call from its own loop and calls the callback's continuation with the value.
// Nothing of the callback is on the C stack while the callee runs, so a pause, a collection that moves
// objects, a resume on another thread and an unwind need nothing of it.
//
// The fixtures:
//   ops       a type whose every eligible operation asks for a function (`f()`, which answers 41) and
//             whose continuation answers by the instance's `mode`: 0 an integer, 1 a decline, 2 an
//             error, 3 DONE, 4 another request (the chain), 5 OK with no answer set, 6 an integer even
//             where a string is wanted (not in the generated test). `ref` is the same
//             type that answers what the request would have without asking: the generated test
//             compares the two for every operation and mode.
//   rx        a type that logs every callback and continuation with the dispatch step it was called at.
//   gen       an iterator that computes each item with a Tang function.
//   view      a callable object whose `call` runs a template.
//   emb       a type whose `equal` and `to_string` ask, reached from inside a container (a C loop).
//   mapn, sortc, each   resumable natives: `mapn(n, f)`, `sortc(a, cmp)` and `each(a, f)`.
//   fwd       a callable object whose `call` asks for the callee in its slot with the arguments it was given.
//   tprint    a type whose `print` asks for the callee in its slot (a template) and prints what it answers.
//   seterr, failnext, keep/kept, peek, framed   natives that mark an instance as an error, fail the next
//             allocation, hold an instance for a `use` path, inspect a value as a debugger would, and ask for a
//             call and then misbehave (below, with the review's tests).
// Each test that depends on the collector forces its arm on inside the test.

#define GLTANG_HARNESS_RUN  // run() in the JIT=no arm too: the differential then compares the interpreter with itself
#include "exec_harness.h"
#include "jit_harness.h"
#include "test_helpers.h"
#include "watchdog.h"

#include <ghoti.io/lang-tang/type.h>

#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace jt;
using tt::Compiled;
using tt::Config;
using tt::Context;

namespace {

constexpr int64_t kF = 41;  ///< What the requested function answers.

struct Bytes {
  int64_t mode;
  int64_t n;
  int64_t pos;
};

/// What a type's callbacks are given as `user`.
struct Fx {
  bool direct = false;                 ///< Answer without asking (the reference).
  std::vector<std::string> * log = nullptr;
  int id = 0;
};

Fx * fx_of(void * user) {
  return static_cast<Fx *>(user);
}

bool read_bytes(GLTANG_NativeCall * call, GLTANG_Handle h, Bytes * b) {
  return gltang_handle_custom_read_bytes(call, h, 0, b, sizeof(*b)) == GLTANG_OK;
}

bool write_bytes(GLTANG_NativeCall * call, GLTANG_Handle h, const Bytes & b) {
  return gltang_handle_custom_write_bytes(call, h, 0, &b, sizeof(b)) == GLTANG_OK;
}

void note(Fx * fx, const std::string & text) {
  if (fx->log) {
    fx->log->push_back(text);
  }
}

std::atomic<int> g_map_conts{0};    ///< How many times mapn's continuation was called.
std::atomic<int> g_bound_conts{0};  ///< How many times a bound native's continuation was called.
std::atomic<int> g_heavy_conts{0};  ///< How many times heavy's continuation was called.
GRCORE_Context * g_ctx = nullptr;   ///< The context install() was last given (peek inspects through it).

// ---------------------------------------------------------------------------
// ops: every eligible operation, requesting
// ---------------------------------------------------------------------------

/// The state of a request: the operation and the cast target, as one integer.
int64_t pack(GLTANG_TypeOp op, int target) {
  return static_cast<int64_t>(op) + 100 * target;
}

/// The answer an operation of the type gives for the number `v`: the shape each operation wants.
GLTANG_OpStatus answer(GLTANG_NativeCall * call, GLTANG_TypeOp op, int target, int64_t v) {
  GLTANG_Handle h;
  switch (op) {
    case GLTANG_TYPE_OP_ITERATOR_BEGIN: {
      GLTANG_Handle x;
      if (gltang_handle_make_array(call, 1, &h) != GLTANG_OK || gltang_handle_make_integer(call, v, &x) != GLTANG_OK || gltang_handle_array_push(call, h, x) != GLTANG_OK) {
        return GLTANG_OP_ERROR;
      }
      gltang_call_return_handle(call, h);
      return GLTANG_OP_OK;
    }
    case GLTANG_TYPE_OP_CAST:
      switch (target) {
        case GLTANG_CAST_TARGET_FLOAT: gltang_call_return_float(call, static_cast<double>(v)); return GLTANG_OP_OK;
        case GLTANG_CAST_TARGET_BOOL: gltang_call_return_bool(call, true); return GLTANG_OP_OK;
        case GLTANG_CAST_TARGET_STRING: break;
        default: gltang_call_return_integer(call, v); return GLTANG_OP_OK;
      }
      /* fall through */
    case GLTANG_TYPE_OP_PRINT:
    case GLTANG_TYPE_OP_TO_STRING: {
      const std::string text = std::to_string(v);
      if (gltang_handle_make_string(call, text.data(), text.size(), GLTANG_UNICODE_STRING_TYPE_TRUSTED, &h) != GLTANG_OK) {
        return GLTANG_OP_ERROR;
      }
      gltang_call_return_handle(call, h);
      return GLTANG_OP_OK;
    }
    default:
      gltang_call_return_integer(call, v);
      return GLTANG_OP_OK;
  }
}

/// What the operation does with the callee's number `value`, by the instance's mode.
GLTANG_OpStatus finish(GLTANG_NativeCall * call, GLTANG_TypeOp op, int target, int64_t mode, int64_t value) {
  switch (mode) {
    case 0: return answer(call, op, target, value + 1);
    case 1: return GLTANG_OP_DECLINE;
    case 2: gltang_call_return_error(call, GLTANG_ERROR_DIVIDE_BY_ZERO); return GLTANG_OP_ERROR;
    case 3: return GLTANG_OP_DONE;
    case 4: return answer(call, op, target, value + 2);
    case 6: gltang_call_return_integer(call, value); return GLTANG_OP_OK;  // an integer, whatever the operation wants
    default: return GLTANG_OP_OK;  // 5: OK with no answer set
  }
}

GLTANG_OpStatus ask(GLTANG_NativeCall * call, GLTANG_Handle f, int64_t state_value, uint32_t continuation) {
  GLTANG_Handle state;
  if (gltang_handle_make_integer(call, state_value, &state) != GLTANG_OK) {
    return GLTANG_OP_ERROR;
  }
  return gltang_call_guest(call, f, nullptr, 0, continuation, state) == GLTANG_OK ? GLTANG_OP_PENDING : GLTANG_OP_ERROR;
}

GLTANG_OpStatus act(GLTANG_NativeCall * call, void * user, GLTANG_Handle self, GLTANG_TypeOp op, int target) {
  Fx * fx = fx_of(user);
  Bytes b;
  if (!read_bytes(call, self, &b)) {
    return GLTANG_OP_ERROR;
  }
  if (fx->direct) {
    return finish(call, op, target, b.mode, b.mode == 4 ? kF : kF);
  }
  GLTANG_Handle f;
  if (gltang_handle_custom_slot_get(call, self, 0, &f) != GLTANG_OK) {
    return GLTANG_OP_ERROR;
  }
  return ask(call, f, pack(op, target), 0);
}

GLTANG_OpStatus cont(GLTANG_NativeCall * call, void *, GLTANG_Handle state, GLTANG_Handle result, bool second) {
  GLTANG_Handle self;
  Bytes b;
  if (gltang_call_arg(call, 0, &self) != GLTANG_OK || !read_bytes(call, self, &b)) {
    return GLTANG_OP_ERROR;
  }
  if (gltang_handle_kind(call, result) == GLTANG_KIND_ERROR) {
    // An error the callee produced is a value: the continuation may return it.
    gltang_call_return_handle(call, result);
    return GLTANG_OP_OK;
  }
  const int64_t packed = gltang_handle_integer(call, state);
  const GLTANG_TypeOp op = static_cast<GLTANG_TypeOp>(packed % 100);
  const int target = static_cast<int>(packed / 100);
  const int64_t value = gltang_handle_integer(call, result);
  if (b.mode == 4 && !second) {
    GLTANG_Handle f;
    if (gltang_handle_custom_slot_get(call, self, 0, &f) != GLTANG_OK) {
      return GLTANG_OP_ERROR;
    }
    return ask(call, f, packed, 1);
  }
  return finish(call, op, target, b.mode, value);
}

GLTANG_OpStatus cont_first(GLTANG_NativeCall * call, void * user, GLTANG_Handle state, GLTANG_Handle result) {
  return cont(call, user, state, result, false);
}

GLTANG_OpStatus cont_second(GLTANG_NativeCall * call, void * user, GLTANG_Handle state, GLTANG_Handle result) {
  return cont(call, user, state, result, true);
}

const GLTANG_Continuation kOpsTable[] = {{"first", cont_first}, {"second", cont_second}};

#define BINARY(name, OP) \
  GLTANG_OpStatus name(GLTANG_NativeCall * c, void * u, GLTANG_Handle s, GLTANG_Handle, bool, bool) { return act(c, u, s, OP, 0); }
#define COMPARE(name, OP) \
  GLTANG_OpStatus name(GLTANG_NativeCall * c, void * u, GLTANG_Handle s, GLTANG_Handle, bool) { return act(c, u, s, OP, 0); }
BINARY(ops_add, GLTANG_TYPE_OP_ADD)
BINARY(ops_sub, GLTANG_TYPE_OP_SUBTRACT)
BINARY(ops_mul, GLTANG_TYPE_OP_MULTIPLY)
BINARY(ops_div, GLTANG_TYPE_OP_DIVIDE)
BINARY(ops_mod, GLTANG_TYPE_OP_MODULO)
COMPARE(ops_lt, GLTANG_TYPE_OP_LESS)
COMPARE(ops_le, GLTANG_TYPE_OP_LESS_EQUAL)
COMPARE(ops_gt, GLTANG_TYPE_OP_GREATER)
COMPARE(ops_ge, GLTANG_TYPE_OP_GREATER_EQUAL)
COMPARE(ops_eq, GLTANG_TYPE_OP_EQUAL)
COMPARE(ops_ne, GLTANG_TYPE_OP_NOT_EQUAL)

GLTANG_OpStatus ops_neg(GLTANG_NativeCall * c, void * u, GLTANG_Handle s) { return act(c, u, s, GLTANG_TYPE_OP_NEGATE, 0); }
GLTANG_OpStatus ops_aget(GLTANG_NativeCall * c, void * u, GLTANG_Handle s, GLTANG_Handle) { return act(c, u, s, GLTANG_TYPE_OP_ATTRIBUTE_GET, 0); }
GLTANG_OpStatus ops_aset(GLTANG_NativeCall * c, void * u, GLTANG_Handle s, GLTANG_Handle, GLTANG_Handle) { return act(c, u, s, GLTANG_TYPE_OP_ATTRIBUTE_SET, 0); }
GLTANG_OpStatus ops_iget(GLTANG_NativeCall * c, void * u, GLTANG_Handle s, GLTANG_Handle) { return act(c, u, s, GLTANG_TYPE_OP_INDEX_GET, 0); }
GLTANG_OpStatus ops_iset(GLTANG_NativeCall * c, void * u, GLTANG_Handle s, GLTANG_Handle, GLTANG_Handle) { return act(c, u, s, GLTANG_TYPE_OP_INDEX_SET, 0); }
GLTANG_OpStatus ops_slice(GLTANG_NativeCall * c, void * u, GLTANG_Handle s, GLTANG_Handle, GLTANG_Handle, GLTANG_Handle) { return act(c, u, s, GLTANG_TYPE_OP_SLICE, 0); }
GLTANG_OpStatus ops_begin(GLTANG_NativeCall * c, void * u, GLTANG_Handle s) { return act(c, u, s, GLTANG_TYPE_OP_ITERATOR_BEGIN, 0); }
GLTANG_OpStatus ops_call(GLTANG_NativeCall * c, void * u, GLTANG_Handle s, const GLTANG_Handle *, size_t) { return act(c, u, s, GLTANG_TYPE_OP_CALL, 0); }
GLTANG_OpStatus ops_cast(GLTANG_NativeCall * c, void * u, GLTANG_Handle s, GLTANG_CastTarget t) { return act(c, u, s, GLTANG_TYPE_OP_CAST, static_cast<int>(t)); }
GLTANG_OpStatus ops_print(GLTANG_NativeCall * c, void * u, GLTANG_Handle s) { return act(c, u, s, GLTANG_TYPE_OP_PRINT, 0); }
GLTANG_OpStatus ops_text(GLTANG_NativeCall * c, void * u, GLTANG_Handle s) { return act(c, u, s, GLTANG_TYPE_OP_TO_STRING, 0); }

constexpr uint32_t kEligible = (1u << GLTANG_TYPE_OP_COUNT) - 1u - (1u << GLTANG_TYPE_OP_TRUTHY) - (1u << GLTANG_TYPE_OP_ITERATOR_NEXT);

GLTANG_TypeDescriptor ops_descriptor(const char * name, Fx * fx, bool resumable) {
  GLTANG_TypeDescriptor d = GLTANG_TYPE_DESCRIPTOR_INIT;
  d.name = name;
  d.slot_count = 1;
  d.byte_count = sizeof(Bytes);
  d.user = fx;
  d.add = ops_add;
  d.subtract = ops_sub;
  d.multiply = ops_mul;
  d.divide = ops_div;
  d.modulo = ops_mod;
  d.negate = ops_neg;
  d.less = ops_lt;
  d.less_equal = ops_le;
  d.greater = ops_gt;
  d.greater_equal = ops_ge;
  d.equal = ops_eq;
  d.not_equal = ops_ne;
  d.attribute_get = ops_aget;
  d.attribute_set = ops_aset;
  d.index_get = ops_iget;
  d.index_set = ops_iset;
  d.slice = ops_slice;
  d.iterator_begin = ops_begin;
  d.call = ops_call;
  d.cast = ops_cast;
  d.print = ops_print;
  d.to_string = ops_text;
  if (resumable) {
    d.resumable_ops = kEligible;
    d.continuations = kOpsTable;
    d.continuation_count = 2;
  }
  return d;
}

// ---------------------------------------------------------------------------
// rx: logs every callback and continuation with its step
// ---------------------------------------------------------------------------

// An instance has an id (bytes.n) and a plan in bytes.mode: what `add`, `not_equal` and `equal` do.
//   0 asks for f, and its continuation answers an integer (the id times 1000, plus 7);
//   1 asks for f, and its continuation declines;
//   2 declines at once (no request);
//   3 asks for f twice, then answers;
//   4 answers at once.
GLTANG_OpStatus rx_entry(GLTANG_NativeCall * call, void * user, GLTANG_Handle self, const char * name) {
  Fx * fx = fx_of(user);
  Bytes b;
  if (!read_bytes(call, self, &b)) {
    return GLTANG_OP_ERROR;
  }
  note(fx, std::string(name) + " " + std::to_string(b.n) + " entry step " + std::to_string(gltang_call_resume_step(call)));
  if (b.mode == 2) {
    return GLTANG_OP_DECLINE;
  }
  if (b.mode == 4) {
    gltang_call_return_integer(call, b.n * 1000 + 3);
    return GLTANG_OP_OK;
  }
  GLTANG_Handle f;
  if (gltang_handle_custom_slot_get(call, self, 0, &f) != GLTANG_OK) {
    return GLTANG_OP_ERROR;
  }
  return ask(call, f, 0, 0);
}

GLTANG_OpStatus rx_cont(GLTANG_NativeCall * call, void * user, GLTANG_Handle state, GLTANG_Handle result) {
  (void)state;
  Fx * fx = fx_of(user);
  GLTANG_Handle self;
  Bytes b;
  if (gltang_call_arg(call, 0, &self) != GLTANG_OK || !read_bytes(call, self, &b)) {
    return GLTANG_OP_ERROR;
  }
  note(fx, "cont " + std::to_string(b.n) + " step " + std::to_string(gltang_call_resume_step(call)) + " result " + std::to_string(gltang_handle_integer(call, result)));
  if (b.mode == 1) {
    return GLTANG_OP_DECLINE;
  }
  if (b.mode == 3 && gltang_handle_integer(call, state) == 0) {
    GLTANG_Handle f;
    if (gltang_handle_custom_slot_get(call, self, 0, &f) != GLTANG_OK) {
      return GLTANG_OP_ERROR;
    }
    return ask(call, f, 1, 0);
  }
  gltang_call_return_integer(call, b.n * 1000 + 7);
  return GLTANG_OP_OK;
}

const GLTANG_Continuation kRxTable[] = {{"rx_cont", rx_cont}};

GLTANG_OpStatus rx_add(GLTANG_NativeCall * c, void * u, GLTANG_Handle s, GLTANG_Handle, bool lhs, bool) {
  return rx_entry(c, u, s, lhs ? "add left" : "add right");
}
GLTANG_OpStatus rx_ne(GLTANG_NativeCall * c, void * u, GLTANG_Handle s, GLTANG_Handle, bool lhs) {
  return rx_entry(c, u, s, lhs ? "not_equal left" : "not_equal right");
}
GLTANG_OpStatus rx_eq(GLTANG_NativeCall * c, void * u, GLTANG_Handle s, GLTANG_Handle, bool lhs) {
  return rx_entry(c, u, s, lhs ? "equal left" : "equal right");
}
GLTANG_OpStatus rx_print(GLTANG_NativeCall * c, void * u, GLTANG_Handle s) {
  return rx_entry(c, u, s, "print");
}
GLTANG_OpStatus rx_text(GLTANG_NativeCall * c, void * u, GLTANG_Handle s) {
  return rx_entry(c, u, s, "to_string");
}
GLTANG_OpStatus rx_cast(GLTANG_NativeCall * c, void * u, GLTANG_Handle s, GLTANG_CastTarget) {
  return rx_entry(c, u, s, "cast");
}

// print and to_string must answer strings: the plan's integer is made into text by wrapping the continuation.
GLTANG_OpStatus rx_text_cont(GLTANG_NativeCall * call, void * user, GLTANG_Handle state, GLTANG_Handle result) {
  Fx * fx = fx_of(user);
  GLTANG_Handle self;
  Bytes b;
  if (gltang_call_arg(call, 0, &self) != GLTANG_OK || !read_bytes(call, self, &b)) {
    return GLTANG_OP_ERROR;
  }
  (void)state;
  note(fx, "cont " + std::to_string(b.n) + " step " + std::to_string(gltang_call_resume_step(call)));
  if (b.mode == 1) {
    return GLTANG_OP_DECLINE;
  }
  return answer(call, GLTANG_TYPE_OP_TO_STRING, 0, b.n * 1000 + 9 + gltang_handle_integer(call, result) * 0);
}

const GLTANG_Continuation kRxTextTable[] = {{"rx_text_cont", rx_text_cont}};

// ---------------------------------------------------------------------------
// gen: an iterator that computes each item with a Tang function
// ---------------------------------------------------------------------------

// bytes.n items; bytes.pos the next index; bytes.mode 1: an item of 13 ends the loop with the error the callee answered.
GLTANG_OpStatus gen_begin(GLTANG_NativeCall * call, void *, GLTANG_Handle self) {
  gltang_call_return_handle(call, self);
  return GLTANG_OP_OK;
}

GLTANG_OpStatus gen_next(GLTANG_NativeCall * call, void *, GLTANG_Handle self) {
  Bytes b;
  if (!read_bytes(call, self, &b)) {
    return GLTANG_OP_ERROR;
  }
  if (b.pos >= b.n) {
    return GLTANG_OP_DONE;
  }
  GLTANG_Handle f, arg, state;
  if (gltang_handle_custom_slot_get(call, self, 0, &f) != GLTANG_OK || gltang_handle_make_integer(call, b.pos, &arg) != GLTANG_OK || gltang_handle_make_integer(call, b.pos, &state) != GLTANG_OK) {
    return GLTANG_OP_ERROR;
  }
  b.pos += 1;
  if (!write_bytes(call, self, b)) {
    return GLTANG_OP_ERROR;
  }
  return gltang_call_guest(call, f, &arg, 1, 0, state) == GLTANG_OK ? GLTANG_OP_PENDING : GLTANG_OP_ERROR;
}

GLTANG_OpStatus gen_cont(GLTANG_NativeCall * call, void *, GLTANG_Handle state, GLTANG_Handle result) {
  (void)state;
  GLTANG_Handle self;
  Bytes b;
  if (gltang_call_arg(call, 0, &self) != GLTANG_OK || !read_bytes(call, self, &b)) {
    return GLTANG_OP_ERROR;
  }
  if (b.mode == 2 && gltang_handle_integer(call, result) == 14) {
    return GLTANG_OP_DONE;  // the continuation ends the loop
  }
  if (gltang_handle_kind(call, result) == GLTANG_KIND_ERROR) {
    gltang_call_return_handle(call, result);
    return GLTANG_OP_ERROR;
  }
  gltang_call_return_handle(call, result);
  return GLTANG_OP_OK;
}

const GLTANG_Continuation kGenTable[] = {{"gen_cont", gen_cont}};

// ---------------------------------------------------------------------------
// view: a callable object that runs a template
// ---------------------------------------------------------------------------

GLTANG_OpStatus view_call(GLTANG_NativeCall * call, void *, GLTANG_Handle self, const GLTANG_Handle *, size_t) {
  GLTANG_Handle t;
  if (gltang_handle_custom_slot_get(call, self, 0, &t) != GLTANG_OK) {
    return GLTANG_OP_ERROR;
  }
  return gltang_call_guest(call, t, nullptr, 0, 0, GLTANG_Handle{0, 0}) == GLTANG_OK ? GLTANG_OP_PENDING : GLTANG_OP_ERROR;
}

GLTANG_OpStatus view_cont(GLTANG_NativeCall * call, void *, GLTANG_Handle, GLTANG_Handle result) {
  gltang_call_return_handle(call, result);
  return GLTANG_OP_OK;
}

const GLTANG_Continuation kViewTable[] = {{"view_cont", view_cont}};

// ---------------------------------------------------------------------------
// emb: a type whose equal and to_string ask, reached from inside a container
// ---------------------------------------------------------------------------

GLTANG_OpStatus emb_eq(GLTANG_NativeCall * call, void * user, GLTANG_Handle self, GLTANG_Handle, bool) {
  Fx * fx = fx_of(user);
  note(fx, "emb equal entry");
  GLTANG_Handle f;
  if (gltang_handle_custom_slot_get(call, self, 0, &f) != GLTANG_OK) {
    return GLTANG_OP_ERROR;
  }
  return ask(call, f, 0, 0);
}

GLTANG_OpStatus emb_eq_cont(GLTANG_NativeCall * call, void * user, GLTANG_Handle, GLTANG_Handle result) {
  note(fx_of(user), "emb equal cont");
  gltang_call_return_bool(call, gltang_handle_integer(call, result) == kF);
  return GLTANG_OP_OK;
}

GLTANG_OpStatus emb_text(GLTANG_NativeCall * call, void * user, GLTANG_Handle self) {
  note(fx_of(user), "emb to_string entry");
  GLTANG_Handle f;
  if (gltang_handle_custom_slot_get(call, self, 0, &f) != GLTANG_OK) {
    return GLTANG_OP_ERROR;
  }
  return ask(call, f, 0, 1);
}

GLTANG_OpStatus emb_text_cont(GLTANG_NativeCall * call, void * user, GLTANG_Handle, GLTANG_Handle result) {
  note(fx_of(user), "emb to_string cont");
  return answer(call, GLTANG_TYPE_OP_TO_STRING, 0, gltang_handle_integer(call, result) + 1);
}

const GLTANG_Continuation kEmbTable[] = {{"emb_eq_cont", emb_eq_cont}, {"emb_text_cont", emb_text_cont}};

// ---------------------------------------------------------------------------
// Natives: mapn, sortc, each
// ---------------------------------------------------------------------------

/// The state of a native's request is an array of integers.
bool make_state(GLTANG_NativeCall * call, const std::vector<int64_t> & values, GLTANG_Handle * out) {
  if (gltang_handle_make_array(call, values.size(), out) != GLTANG_OK) {
    return false;
  }
  for (int64_t v : values) {
    GLTANG_Handle x;
    if (gltang_handle_make_integer(call, v, &x) != GLTANG_OK || gltang_handle_array_push(call, *out, x) != GLTANG_OK) {
      return false;
    }
  }
  return true;
}

bool read_state(GLTANG_NativeCall * call, GLTANG_Handle state, std::vector<int64_t> * values) {
  const size_t n = gltang_handle_array_length(call, state);
  if (gltang_handle_kind(call, state) != GLTANG_KIND_ARRAY || n > 4096u) {
    return false;  // not the array this native made: a state the collector freed reads as nonsense, and fails fast
  }
  values->clear();
  for (size_t i = 0; i < n; ++i) {
    GLTANG_Handle x;
    if (gltang_handle_array_get(call, state, i, &x) != GLTANG_OK) {
      return false;
    }
    values->push_back(gltang_handle_integer(call, x));
  }
  return true;
}

bool return_ints(GLTANG_NativeCall * call, const std::vector<int64_t> & values) {
  GLTANG_Handle h;
  if (!make_state(call, values, &h)) {
    return false;
  }
  gltang_call_return_handle(call, h);
  return true;
}

// mapn(n, f): [f(0), ..., f(n - 1)]. The state is the results so far; the arguments come again with the continuation.
bool request_call(GLTANG_NativeCall * call, GLTANG_Handle f, const std::vector<int64_t> & args, const std::vector<int64_t> & state, uint32_t continuation) {
  GLTANG_Handle handles[4];
  for (size_t i = 0; i < args.size(); ++i) {
    if (gltang_handle_make_integer(call, args[i], &handles[i]) != GLTANG_OK) {
      return false;
    }
  }
  GLTANG_Handle s;
  if (!make_state(call, state, &s)) {
    return false;
  }
  return gltang_call_guest(call, f, handles, args.size(), continuation, s) == GLTANG_OK;
}

bool mapn(GLTANG_NativeCall * call, void *) {
  GLTANG_Handle f;
  const int64_t n = gltang_call_integer(call, 0);
  if (gltang_call_count(call) != 2 || gltang_call_arg(call, 1, &f) != GLTANG_OK) {
    return false;
  }
  if (n <= 0) {
    return return_ints(call, {});
  }
  return request_call(call, f, {0}, {}, 0);
}

GLTANG_OpStatus mapn_cont(GLTANG_NativeCall * call, void *, GLTANG_Handle state, GLTANG_Handle result) {
  ++g_map_conts;
  std::vector<int64_t> results;
  GLTANG_Handle f;
  if (!read_state(call, state, &results) || gltang_call_arg(call, 1, &f) != GLTANG_OK) {
    return GLTANG_OP_ERROR;
  }
  if (gltang_handle_kind(call, result) != GLTANG_KIND_INTEGER) {
    gltang_call_return_handle(call, result);  // the callee's error is the native's value
    return GLTANG_OP_OK;
  }
  results.push_back(gltang_handle_integer(call, result));
  if (static_cast<int64_t>(results.size()) >= gltang_call_integer(call, 0)) {
    return return_ints(call, results) ? GLTANG_OP_OK : GLTANG_OP_ERROR;
  }
  return request_call(call, f, {static_cast<int64_t>(results.size())}, results, 0) ? GLTANG_OP_PENDING : GLTANG_OP_ERROR;
}

const GLTANG_Continuation kMapTable[] = {{"mapn_cont", mapn_cont}};

// each(a, f): f(a[i]) for each element, in order; null.
bool element(GLTANG_NativeCall * call, size_t arg, size_t index, int64_t * out) {
  GLTANG_Handle a, x;
  if (gltang_call_arg(call, arg, &a) != GLTANG_OK || gltang_handle_array_get(call, a, index, &x) != GLTANG_OK) {
    return false;
  }
  *out = gltang_handle_integer(call, x);
  return true;
}

size_t length_of(GLTANG_NativeCall * call, size_t arg) {
  GLTANG_Handle a;
  return gltang_call_arg(call, arg, &a) == GLTANG_OK ? gltang_handle_array_length(call, a) : 0;
}

bool each(GLTANG_NativeCall * call, void *) {
  GLTANG_Handle f;
  int64_t first;
  if (gltang_call_count(call) != 2 || gltang_call_arg(call, 1, &f) != GLTANG_OK) {
    return false;
  }
  if (length_of(call, 0) == 0) {
    gltang_call_return_null(call);
    return true;
  }
  return element(call, 0, 0, &first) && request_call(call, f, {first}, {0}, 0);
}

GLTANG_OpStatus each_cont(GLTANG_NativeCall * call, void *, GLTANG_Handle state, GLTANG_Handle) {
  std::vector<int64_t> s;
  GLTANG_Handle f;
  int64_t next;
  if (!read_state(call, state, &s) || gltang_call_arg(call, 1, &f) != GLTANG_OK) {
    return GLTANG_OP_ERROR;
  }
  const size_t i = static_cast<size_t>(s[0]) + 1u;
  if (i >= length_of(call, 0)) {
    gltang_call_return_null(call);
    return GLTANG_OP_OK;
  }
  return element(call, 0, i, &next) && request_call(call, f, {next}, {static_cast<int64_t>(i)}, 0) ? GLTANG_OP_PENDING : GLTANG_OP_ERROR;
}

const GLTANG_Continuation kEachTable[] = {{"each_cont", each_cont}};

// sortc(a, cmp): the elements of a (integers) in the order cmp(x, y) (true when x comes before y) gives, by selection:
// one request per comparison. The state is [mask of taken, best, current, out...].
struct Sort {
  int64_t mask = 0, best = -1, current = -1;
  std::vector<int64_t> out;
};

enum class Step { Ask, Done, Error };

Step advance(GLTANG_NativeCall * call, Sort * s, int64_t * x, int64_t * y) {
  const size_t n = length_of(call, 0);
  for (;;) {
    // The next candidate after `current` that is not taken.
    size_t i = static_cast<size_t>(s->current + 1);
    while (i < n && ((s->mask >> i) & 1)) {
      ++i;
    }
    if (i >= n) {
      if (s->best < 0) {
        return Step::Done;
      }
      int64_t v;
      if (!element(call, 0, static_cast<size_t>(s->best), &v)) {
        return Step::Error;
      }
      s->out.push_back(v);
      s->mask |= int64_t{1} << s->best;
      s->best = -1;
      s->current = -1;
      continue;
    }
    s->current = static_cast<int64_t>(i);
    if (s->best < 0) {
      s->best = s->current;
      continue;
    }
    return element(call, 0, i, x) && element(call, 0, static_cast<size_t>(s->best), y) ? Step::Ask : Step::Error;
  }
}

std::vector<int64_t> pack_sort(const Sort & s) {
  std::vector<int64_t> v = {s.mask, s.best, s.current};
  v.insert(v.end(), s.out.begin(), s.out.end());
  return v;
}

bool sort_step(GLTANG_NativeCall * call, Sort * s, bool * done) {
  int64_t x = 0, y = 0;
  *done = false;
  switch (advance(call, s, &x, &y)) {
    case Step::Done:
      *done = true;
      return return_ints(call, s->out);
    case Step::Error:
      return false;
    case Step::Ask: {
      GLTANG_Handle cmp;
      return gltang_call_arg(call, 1, &cmp) == GLTANG_OK && request_call(call, cmp, {x, y}, pack_sort(*s), 0);
    }
  }
  return false;
}

bool sortc(GLTANG_NativeCall * call, void *) {
  Sort s;
  bool done;
  return gltang_call_count(call) == 2 && sort_step(call, &s, &done);
}

GLTANG_OpStatus sort_cont(GLTANG_NativeCall * call, void *, GLTANG_Handle state, GLTANG_Handle result) {
  std::vector<int64_t> v;
  if (!read_state(call, state, &v) || v.size() < 3) {
    return GLTANG_OP_ERROR;
  }
  if (gltang_handle_kind(call, result) == GLTANG_KIND_ERROR) {
    gltang_call_return_handle(call, result);
    return GLTANG_OP_OK;
  }
  Sort s;
  s.mask = v[0];
  s.best = v[1];
  s.current = v[2];
  s.out.assign(v.begin() + 3, v.end());
  if (gltang_handle_kind(call, result) == GLTANG_KIND_BOOL ? gltang_handle_bool(call, result) : gltang_handle_integer(call, result) != 0) {
    s.best = s.current;
  }
  bool done;
  if (!sort_step(call, &s, &done)) {
    return GLTANG_OP_ERROR;
  }
  return done ? GLTANG_OP_OK : GLTANG_OP_PENDING;
}

const GLTANG_Continuation kSortTable[] = {{"sort_cont", sort_cont}};

// bind(f): a resumable native bound to f; called with x it asks for f(x), and its continuation adds 1000.
bool bound_call(GLTANG_NativeCall * call, void *) {
  GLTANG_Handle f, x;
  if (gltang_call_bound(call, &f) != GLTANG_OK || gltang_call_arg(call, 0, &x) != GLTANG_OK) {
    return false;
  }
  return gltang_call_guest(call, f, &x, 1, 0, GLTANG_Handle{0, 0}) == GLTANG_OK;
}

GLTANG_OpStatus bound_cont(GLTANG_NativeCall * call, void *, GLTANG_Handle, GLTANG_Handle result) {
  ++g_bound_conts;
  if (gltang_handle_kind(call, result) != GLTANG_KIND_INTEGER) {
    gltang_call_return_handle(call, result);
    return GLTANG_OP_OK;
  }
  gltang_call_return_integer(call, gltang_handle_integer(call, result) + 1000);
  return GLTANG_OP_OK;
}

const GLTANG_Continuation kBoundTable[] = {{"bound_cont", bound_cont}};

bool bind(GLTANG_NativeCall * call, void *) {
  GLTANG_Handle f, h;
  if (gltang_call_arg(call, 0, &f) != GLTANG_OK || gltang_handle_make_resumable_native(call, bound_call, nullptr, kBoundTable, 1, f, &h) != GLTANG_OK) {
    return false;
  }
  gltang_call_return_handle(call, h);
  return true;
}

// A host attribute of a built-in kind (`s.apply(f)`) is never resumable itself; it answers a bound resumable native.
bool apply_native(GLTANG_NativeCall * call, void *) {
  GLTANG_Handle f, bound;
  if (gltang_call_arg(call, 0, &f) != GLTANG_OK || gltang_call_bound(call, &bound) != GLTANG_OK) {
    return false;
  }
  return gltang_call_guest(call, f, &bound, 1, 0, GLTANG_Handle{0, 0}) == GLTANG_OK;
}

GLTANG_OpStatus apply_cont(GLTANG_NativeCall * call, void *, GLTANG_Handle, GLTANG_Handle result) {
  gltang_call_return_handle(call, result);
  return GLTANG_OP_OK;
}

const GLTANG_Continuation kApplyTable[] = {{"apply_cont", apply_cont}};

GLTANG_OpStatus attr_apply(GLTANG_NativeCall * call, void *, GLTANG_Handle self) {
  GLTANG_Handle h;
  if (gltang_handle_make_resumable_native(call, apply_native, nullptr, kApplyTable, 1, self, &h) != GLTANG_OK) {
    return GLTANG_OP_ERROR;
  }
  gltang_call_return_handle(call, h);
  return GLTANG_OP_OK;
}

// ---------------------------------------------------------------------------
// fwd, tprint and the natives the review's tests use
// ---------------------------------------------------------------------------

GLTANG_OpStatus fwd_call(GLTANG_NativeCall * call, void *, GLTANG_Handle self, const GLTANG_Handle * args, size_t argc) {
  GLTANG_Handle f;
  if (gltang_handle_custom_slot_get(call, self, 0, &f) != GLTANG_OK) {
    return GLTANG_OP_ERROR;
  }
  return gltang_call_guest(call, f, args, argc, 0, GLTANG_Handle{0, 0}) == GLTANG_OK ? GLTANG_OP_PENDING : GLTANG_OP_ERROR;
}

GLTANG_OpStatus tprint_print(GLTANG_NativeCall * call, void *, GLTANG_Handle self) {
  GLTANG_Handle t;
  if (gltang_handle_custom_slot_get(call, self, 0, &t) != GLTANG_OK) {
    return GLTANG_OP_ERROR;
  }
  return gltang_call_guest(call, t, nullptr, 0, 0, GLTANG_Handle{0, 0}) == GLTANG_OK ? GLTANG_OP_PENDING : GLTANG_OP_ERROR;
}

/// seterr(x): x, an instance, stands for an error from now on (null).
bool seterr(GLTANG_NativeCall * call, void *) {
  GLTANG_Handle x;
  return gltang_call_arg(call, 0, &x) == GLTANG_OK && gltang_handle_custom_set_error(call, x, true) == GLTANG_OK;
}

/// failnext(n): the nth allocation the process makes from now on fails (the library's own through cutil included).
bool failnext(GLTANG_NativeCall * call, void *) {
  alloc_sweep::arm(static_cast<long>(gltang_call_integer(call, 0)));
  return true;
}

/// keep(x): x is held in a persistent handle, which the handle factory `kept` answers (a `use t.kept.name` path).
GLTANG_Persistent g_kept = {0, 0};

bool keep(GLTANG_NativeCall * call, void *) {
  GLTANG_Handle x;
  return gltang_call_arg(call, 0, &x) == GLTANG_OK && gltang_handle_promote(call, x, &g_kept) == GLTANG_OK;
}

bool kept(GLTANG_NativeCall * call, void *) {
  GLTANG_Handle h;
  if (gltang_persistent_get(call, g_kept, &h) != GLTANG_OK) {
    return false;
  }
  gltang_call_return_handle(call, h);
  return true;
}

/// peek(x): the text the engine's inspector gives for x, asked from inside a native as a debugger at a poll would ask.
bool peek(GLTANG_NativeCall * call, void *) {
  GLTANG_Handle x;
  if (gltang_call_arg(call, 0, &x) != GLTANG_OK) {
    return false;
  }
  void * object = gltang_vm_test_handle_object(call, x);
  char text[256] = {0};
  size_t length = 0;
  GRCORE_Stack * stack = grcore_context_stack(g_ctx);
  GRCORE_EngineId engine;
  if (!object || grcore_stack_engine(stack, grcore_stack_top(stack), &engine) != GRCORE_OK ||
      grcore_engine_inspect(g_ctx, engine, GRCORE_SLOT_VALUE, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(object)), text, sizeof text, &length) != GRCORE_OK) {
    return false;
  }
  text[sizeof text - 1u] = '\0';
  gltang_call_return_string(call, text, std::strlen(text), GLTANG_UNICODE_STRING_TYPE_TRUSTED);
  return true;
}

/// framed(f): asks for f(a) with a state, both arrays made in a handle frame that it pops after asking, and then makes
/// forty arrays. A callback must touch nothing but its return after asking; one that does must not see what it asked
/// for collected under it.
bool framed(GLTANG_NativeCall * call, void *) {
  GLTANG_Handle f, a, s;
  if (gltang_call_arg(call, 0, &f) != GLTANG_OK || gltang_handle_push_frame(call) != GLTANG_OK || !make_state(call, {1, 2, 3}, &a) ||
      !make_state(call, {7, 8, 9}, &s) || gltang_call_guest(call, f, &a, 1, 0, s) != GLTANG_OK || gltang_handle_pop_frame(call) != GLTANG_OK) {
    return false;
  }
  for (int64_t i = 0; i < 40; ++i) {
    GLTANG_Handle junk;
    if (!make_state(call, {i, i + 1, i + 2}, &junk)) {
      return false;
    }
  }
  return true;
}

GLTANG_OpStatus framed_cont(GLTANG_NativeCall * call, void *, GLTANG_Handle state, GLTANG_Handle result) {
  std::vector<int64_t> s;
  if (!read_state(call, state, &s) || s.size() != 3u || gltang_handle_kind(call, result) != GLTANG_KIND_INTEGER) {
    return GLTANG_OP_ERROR;
  }
  gltang_call_return_integer(call, gltang_handle_integer(call, result) * 100 + s[0] + s[1] + s[2]);
  return GLTANG_OP_OK;
}

const GLTANG_Continuation kFramedTable[] = {{"framed_cont", framed_cont}};

/// heavy(f): asks for f(1) with a state of 64 KiB (a string), so that a record that survives its run is seen in the
/// bytes the heap holds; its continuation answers the callee's value.
bool heavy(GLTANG_NativeCall * call, void *) {
  static const std::string big(65536, 'x');
  GLTANG_Handle f, s, x;
  return gltang_call_arg(call, 0, &f) == GLTANG_OK && gltang_handle_make_string(call, big.data(), big.size(), GLTANG_UNICODE_STRING_TYPE_TRUSTED, &s) == GLTANG_OK &&
         gltang_handle_make_integer(call, 1, &x) == GLTANG_OK && gltang_call_guest(call, f, &x, 1, 0, s) == GLTANG_OK;
}

GLTANG_OpStatus heavy_cont(GLTANG_NativeCall * call, void *, GLTANG_Handle, GLTANG_Handle result) {
  ++g_heavy_conts;
  gltang_call_return_handle(call, result);
  return GLTANG_OP_OK;
}

const GLTANG_Continuation kHeavyTable[] = {{"heavy_cont", heavy_cont}};

// ---------------------------------------------------------------------------
// The fixture a test builds: the types, the natives and what they log
// ---------------------------------------------------------------------------

struct Types {
  GLTANG_TypeRegistry * registry = nullptr;
  const GLTANG_Type * req = nullptr;
  const GLTANG_Type * ref = nullptr;
  const GLTANG_Type * refr = nullptr;  ///< ref, declared resumable (and never asking)
  const GLTANG_Type * rx = nullptr;
  const GLTANG_Type * rxtext = nullptr;
  const GLTANG_Type * gen = nullptr;
  const GLTANG_Type * view = nullptr;
  const GLTANG_Type * emb = nullptr;
  const GLTANG_Type * fwd = nullptr;
  const GLTANG_Type * tprint = nullptr;
  Fx fx_req, fx_ref, fx_log;
  std::vector<std::string> log;
  Types() {
    EXPECT_EQ(gltang_type_registry_create(&registry), GLTANG_OK);
    fx_req.direct = false;
    fx_ref.direct = true;
    fx_log.log = &log;
    GLTANG_TypeDescriptor d = ops_descriptor("ops", &fx_req, true);
    EXPECT_EQ(gltang_type_registry_add(registry, &d, &req), GLTANG_OK);
    d = ops_descriptor("ref", &fx_ref, false);
    EXPECT_EQ(gltang_type_registry_add(registry, &d, &ref), GLTANG_OK);
    d = ops_descriptor("refr", &fx_ref, true);
    EXPECT_EQ(gltang_type_registry_add(registry, &d, &refr), GLTANG_OK);

    d = GLTANG_TYPE_DESCRIPTOR_INIT;
    d.name = "rx";
    d.slot_count = 1;
    d.byte_count = sizeof(Bytes);
    d.user = &fx_log;
    d.add = rx_add;
    d.not_equal = rx_ne;
    d.equal = rx_eq;
    d.cast = rx_cast;
    d.resumable_ops = (1u << GLTANG_TYPE_OP_ADD) | (1u << GLTANG_TYPE_OP_NOT_EQUAL) | (1u << GLTANG_TYPE_OP_EQUAL) | (1u << GLTANG_TYPE_OP_CAST);
    d.continuations = kRxTable;
    d.continuation_count = 1;
    EXPECT_EQ(gltang_type_registry_add(registry, &d, &rx), GLTANG_OK);

    d = GLTANG_TYPE_DESCRIPTOR_INIT;
    d.name = "rxtext";
    d.slot_count = 1;
    d.byte_count = sizeof(Bytes);
    d.user = &fx_log;
    d.print = rx_print;
    d.to_string = rx_text;
    d.resumable_ops = (1u << GLTANG_TYPE_OP_PRINT) | (1u << GLTANG_TYPE_OP_TO_STRING);
    d.continuations = kRxTextTable;
    d.continuation_count = 1;
    EXPECT_EQ(gltang_type_registry_add(registry, &d, &rxtext), GLTANG_OK);

    d = GLTANG_TYPE_DESCRIPTOR_INIT;
    d.name = "gen";
    d.slot_count = 1;
    d.byte_count = sizeof(Bytes);
    d.iterator_begin = gen_begin;
    d.iterator_next = gen_next;
    d.resumable_ops = 1u << GLTANG_TYPE_OP_ITERATOR_NEXT;
    d.continuations = kGenTable;
    d.continuation_count = 1;
    EXPECT_EQ(gltang_type_registry_add(registry, &d, &gen), GLTANG_OK);

    d = GLTANG_TYPE_DESCRIPTOR_INIT;
    d.name = "view";
    d.slot_count = 1;
    d.call = view_call;
    d.resumable_ops = 1u << GLTANG_TYPE_OP_CALL;
    d.continuations = kViewTable;
    d.continuation_count = 1;
    EXPECT_EQ(gltang_type_registry_add(registry, &d, &view), GLTANG_OK);

    EXPECT_EQ(gltang_type_registry_add_attribute(registry, GLTANG_KIND_STRING, "apply", attr_apply, nullptr), GLTANG_OK);

    d = GLTANG_TYPE_DESCRIPTOR_INIT;
    d.name = "emb";
    d.slot_count = 1;
    d.user = &fx_log;
    d.equal = emb_eq;
    d.to_string = emb_text;
    d.resumable_ops = (1u << GLTANG_TYPE_OP_EQUAL) | (1u << GLTANG_TYPE_OP_TO_STRING);
    d.continuations = kEmbTable;
    d.continuation_count = 2;
    EXPECT_EQ(gltang_type_registry_add(registry, &d, &emb), GLTANG_OK);

    d = GLTANG_TYPE_DESCRIPTOR_INIT;
    d.name = "fwd";
    d.slot_count = 1;
    d.call = fwd_call;
    d.resumable_ops = 1u << GLTANG_TYPE_OP_CALL;
    d.continuations = kViewTable;
    d.continuation_count = 1;
    EXPECT_EQ(gltang_type_registry_add(registry, &d, &fwd), GLTANG_OK);

    d = GLTANG_TYPE_DESCRIPTOR_INIT;
    d.name = "tprint";
    d.slot_count = 1;
    d.print = tprint_print;
    d.resumable_ops = 1u << GLTANG_TYPE_OP_PRINT;
    d.continuations = kViewTable;
    d.continuation_count = 1;
    EXPECT_EQ(gltang_type_registry_add(registry, &d, &tprint), GLTANG_OK);
  }
  ~Types() { gltang_type_registry_release(registry); }
  Types(const Types &) = delete;
  Types & operator=(const Types &) = delete;
};

/// make(type)(f, mode, n): an instance with the function in its slot.
template <const GLTANG_Type * Types::*Member>
bool make_instance(GLTANG_NativeCall * call, void * user) {
  Types * t = static_cast<Types *>(user);
  GLTANG_Handle h, f;
  Bytes b = {0, 0, 0};
  if (gltang_call_count(call) > 1) {
    b.mode = gltang_call_integer(call, 1);
  }
  if (gltang_call_count(call) > 2) {
    b.n = gltang_call_integer(call, 2);
  }
  if (gltang_handle_make_custom(call, t->*Member, &h) != GLTANG_OK || (gltang_type_byte_count(t->*Member) != 0 && !write_bytes(call, h, b))) {
    return false;
  }
  if (gltang_call_count(call) > 0) {
    if (gltang_call_arg(call, 0, &f) != GLTANG_OK || gltang_handle_custom_slot_set(call, h, 0, f) != GLTANG_OK) {
      return false;
    }
  }
  gltang_call_return_handle(call, h);
  return true;
}

bool log_note(GLTANG_NativeCall * call, void * user) {
  Types * t = static_cast<Types *>(user);
  t->log.push_back("f " + std::to_string(gltang_call_integer(call, 0)));
  gltang_call_return_integer(call, gltang_call_integer(call, 0));
  return true;
}

/// Attaches the types and the natives to a context.
void install(Context & context, Types & t) {
  g_ctx = context.context;
  context.add_native_library();
  GLTANG_Library * lib = nullptr;
  EXPECT_EQ(gltang_library_create("t", &lib), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_native(lib, "ops", make_instance<&Types::req>, &t), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_native(lib, "ref", make_instance<&Types::ref>, &t), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_native(lib, "refr", make_instance<&Types::refr>, &t), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_native(lib, "rx", make_instance<&Types::rx>, &t), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_native(lib, "rxtext", make_instance<&Types::rxtext>, &t), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_native(lib, "gen", make_instance<&Types::gen>, &t), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_native(lib, "view", make_instance<&Types::view>, &t), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_native(lib, "emb", make_instance<&Types::emb>, &t), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_native(lib, "fwd", make_instance<&Types::fwd>, &t), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_native(lib, "tprint", make_instance<&Types::tprint>, &t), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_native(lib, "seterr", seterr, nullptr), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_native(lib, "failnext", failnext, nullptr), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_native(lib, "keep", keep, nullptr), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_handle_factory(lib, "kept", kept, nullptr), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_native(lib, "peek", peek, nullptr), GLTANG_OK);
  EXPECT_EQ(gltang_library_mark_resumable(lib, "peek", kViewTable, 1), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_native(lib, "peekplain", peek, nullptr), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_native(lib, "framed", framed, nullptr), GLTANG_OK);
  EXPECT_EQ(gltang_library_mark_resumable(lib, "framed", kFramedTable, 1), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_native(lib, "heavy", heavy, nullptr), GLTANG_OK);
  EXPECT_EQ(gltang_library_mark_resumable(lib, "heavy", kHeavyTable, 1), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_native(lib, "note", log_note, &t), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_native(lib, "bind", bind, nullptr), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_native(lib, "mapn", mapn, nullptr), GLTANG_OK);
  EXPECT_EQ(gltang_library_mark_resumable(lib, "mapn", kMapTable, 1), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_native(lib, "each", each, nullptr), GLTANG_OK);
  EXPECT_EQ(gltang_library_mark_resumable(lib, "each", kEachTable, 1), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_native(lib, "sortc", sortc, nullptr), GLTANG_OK);
  EXPECT_EQ(gltang_library_mark_resumable(lib, "sortc", kSortTable, 1), GLTANG_OK);
  EXPECT_EQ(gltang_library_add_library(context.library(), lib), GLTANG_OK);
  gltang_library_release(lib);
  EXPECT_EQ(gltang_execution_attach_types(context.execution, t.registry), GLTANG_OK);
}

struct Result {
  Outcome out;
  std::vector<std::string> log;
  uint64_t pending_depth = 0;
  uint64_t nested = 0;
  uint64_t suspended = 0;
};

/// Runs a script at a JIT threshold, with the types installed.
Result go(Scenario sc, long threshold) {
  Types types;
  Result r;
  auto before = sc.before;
  sc.before = [&](Context & c) {
    install(c, types);
    if (before) {
      before(c);
    }
  };
  auto after = sc.after;
  sc.after = [&](Context & c) {
    r.pending_depth = gltang_vm_test_pending_depth(c.execution);
    gltang_vm_test_request_counts(c.execution, &r.nested, &r.suspended);
    if (after) {
      after(c);
    }
  };
  r.out = run(sc, threshold);
  r.log = types.log;
  return r;
}

Scenario scenario(const std::string & source) {
  Scenario sc;
  sc.source = source;
  sc.script = true;
  return sc;
}

constexpr uint64_t kFuelMapThree = 32;
constexpr uint64_t kFuelAdd = 35;
constexpr uint64_t kFuelChainExtra = 5;

const char * kPrelude = "use t; ops = t.ops; ref = t.ref; function f() { return 41; }\n";

}  // namespace

// ---------------------------------------------------------------------------
// The API and its refusals
// ---------------------------------------------------------------------------

TEST(ReentryApi, ADescriptorThatDeclaresNoGuestCallsHashesAsItAlwaysDidAndOneThatDoesDiffers) {
  GLTANG_TypeRegistry * r = nullptr;
  ASSERT_EQ(gltang_type_registry_create(&r), GLTANG_OK);
  GLTANG_TypeDescriptor d = GLTANG_TYPE_DESCRIPTOR_INIT;
  d.name = "h";
  d.add = ops_add;
  const GLTANG_Type * plain = nullptr;
  ASSERT_EQ(gltang_type_registry_add(r, &d, &plain), GLTANG_OK);
  // The same descriptor sized for the first version of the descriptor (story 6): every later member is read as empty.
  GLTANG_TypeDescriptor old = d;
  old.name = "h_old";
  old.size = offsetof(GLTANG_TypeDescriptor, to_string) + sizeof(GLTANG_TypeToStringFn);
  const GLTANG_Type * old_type = nullptr;
  ASSERT_EQ(gltang_type_registry_add(r, &old, &old_type), GLTANG_OK);
  d.name = "r";
  d.resumable_ops = 1u << GLTANG_TYPE_OP_ADD;
  d.continuations = kOpsTable;
  d.continuation_count = 2;
  const GLTANG_Type * resumable = nullptr;
  ASSERT_EQ(gltang_type_registry_add(r, &d, &resumable), GLTANG_OK);
  d.name = "r2";
  d.continuation_count = 1;
  const GLTANG_Type * fewer = nullptr;
  ASSERT_EQ(gltang_type_registry_add(r, &d, &fewer), GLTANG_OK);
  EXPECT_NE(gltang_type_table_hash(resumable), gltang_type_table_hash(fewer));
  EXPECT_NE(gltang_type_table_hash(plain), gltang_type_table_hash(resumable));
  // A type with the same name and declaration hashes the same in another registry.
  GLTANG_TypeRegistry * r2 = nullptr;
  ASSERT_EQ(gltang_type_registry_create(&r2), GLTANG_OK);
  d.name = "r";
  d.continuation_count = 2;
  const GLTANG_Type * again = nullptr;
  ASSERT_EQ(gltang_type_registry_add(r2, &d, &again), GLTANG_OK);
  EXPECT_EQ(gltang_type_table_hash(resumable), gltang_type_table_hash(again));
  d.name = "h";
  d.resumable_ops = 0;
  d.continuations = nullptr;
  d.continuation_count = 0;
  const GLTANG_Type * plain2 = nullptr;
  ASSERT_EQ(gltang_type_registry_add(r2, &d, &plain2), GLTANG_OK);
  EXPECT_EQ(gltang_type_table_hash(plain), gltang_type_table_hash(plain2));
  EXPECT_EQ(gltang_type_table_hash(plain), gltang_type_table_hash(old_type) ^ gltang_type_table_hash(plain) ^ gltang_type_table_hash(old_type)) << "names differ in the hash, but the declaration beyond the name does not";
  gltang_type_registry_release(r);
  gltang_type_registry_release(r2);
}

TEST(ReentryApi, TheMaskTheTableAndTheirPartsAreCheckedAtRegistration) {
  GLTANG_TypeRegistry * r = nullptr;
  ASSERT_EQ(gltang_type_registry_create(&r), GLTANG_OK);
  auto base = [](const char * name) {
    GLTANG_TypeDescriptor d = GLTANG_TYPE_DESCRIPTOR_INIT;
    d.name = name;
    d.add = ops_add;
    d.truthy = nullptr;
    d.continuations = kOpsTable;
    d.continuation_count = 2;
    return d;
  };
  GLTANG_TypeDescriptor d = base("a");
  d.resumable_ops = 1u << GLTANG_TYPE_OP_ADD;
  EXPECT_EQ(gltang_type_registry_add(r, &d, nullptr), GLTANG_OK);
  d = base("b");
  d.resumable_ops = 1u << GLTANG_TYPE_OP_SUBTRACT;
  EXPECT_EQ(gltang_type_registry_add(r, &d, nullptr), GLTANG_ERR_INVALID) << "a bit for an operation the type does not implement";
  d = base("c");
  d.truthy = [](GLTANG_NativeCall *, void *, GLTANG_Handle, bool *) { return GLTANG_OP_OK; };
  d.resumable_ops = 1u << GLTANG_TYPE_OP_TRUTHY;
  EXPECT_EQ(gltang_type_registry_add(r, &d, nullptr), GLTANG_ERR_INVALID) << "truthy is never resumable";
  d = base("d");
  d.resumable_ops = 1u << GLTANG_TYPE_OP_COUNT;
  EXPECT_EQ(gltang_type_registry_add(r, &d, nullptr), GLTANG_ERR_INVALID) << "a bit past the last operation";
  d = base("e");
  d.resumable_ops = 1u << GLTANG_TYPE_OP_ADD;
  d.continuations = nullptr;
  d.continuation_count = 0;
  EXPECT_EQ(gltang_type_registry_add(r, &d, nullptr), GLTANG_ERR_INVALID) << "a mask with no table";
  d = base("f");
  d.resumable_ops = 1u << GLTANG_TYPE_OP_ADD;
  GLTANG_Continuation dup[2] = {{"x", cont_first}, {"x", cont_second}};
  d.continuations = dup;
  EXPECT_EQ(gltang_type_registry_add(r, &d, nullptr), GLTANG_ERR_INVALID) << "a repeated name";
  GLTANG_Continuation nofn[1] = {{"x", nullptr}};
  d.continuations = nofn;
  d.continuation_count = 1;
  EXPECT_EQ(gltang_type_registry_add(r, &d, nullptr), GLTANG_ERR_INVALID) << "a table entry with no function";
  GLTANG_Continuation badname[1] = {{"1x", cont_first}};
  d.continuations = badname;
  EXPECT_EQ(gltang_type_registry_add(r, &d, nullptr), GLTANG_ERR_INVALID) << "a name that is not an identifier";
  d.continuations = kOpsTable;
  d.continuation_count = GLTANG_CONTINUATIONS_MAX + 1u;
  EXPECT_EQ(gltang_type_registry_add(r, &d, nullptr), GLTANG_ERR_INVALID) << "a table past its maximum";
  gltang_type_registry_release(r);

  GLTANG_Library * lib = nullptr;
  ASSERT_EQ(gltang_library_create("m", &lib), GLTANG_OK);
  ASSERT_EQ(gltang_library_add_native(lib, "n", mapn, nullptr), GLTANG_OK);
  ASSERT_EQ(gltang_library_add_integer(lib, "i", 1), GLTANG_OK);
  EXPECT_EQ(gltang_library_mark_resumable(lib, "missing", kMapTable, 1), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_library_mark_resumable(lib, "i", kMapTable, 1), GLTANG_ERR_INVALID) << "only a native";
  EXPECT_EQ(gltang_library_mark_resumable(lib, "n", kMapTable, 0), GLTANG_ERR_INVALID) << "an empty table";
  EXPECT_EQ(gltang_library_mark_resumable(lib, "n", nullptr, 1), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_library_mark_resumable(lib, "n", kMapTable, 1), GLTANG_OK);
  EXPECT_EQ(gltang_library_mark_resumable(lib, "n", kMapTable, 1), GLTANG_ERR_INVALID) << "marked already";
  GLTANG_Library * root = nullptr;
  ASSERT_EQ(gltang_library_create(nullptr, &root), GLTANG_OK);
  ASSERT_EQ(gltang_library_add_library(root, lib), GLTANG_OK);
  ASSERT_EQ(gltang_library_add_native(lib, "late", mapn, nullptr), GLTANG_ERR_INVALID) << "sealed";
  EXPECT_EQ(gltang_library_mark_resumable(lib, "n", kMapTable, 1), GLTANG_ERR_INVALID) << "a sealed library";
  gltang_library_release(root);
  gltang_library_release(lib);
}

// ---------------------------------------------------------------------------
// The matrix: collection natives
// ---------------------------------------------------------------------------

TEST(Reentry, MapRunsTheFunctionPerElementAndAnswersTheMappedArray) {
  Scenario sc = scenario("use t; function sq(x) { return x * x + 1; } r = t.mapn(4, sq); print(r[0]); print(\",\"); print(r[3]); print(\",\"); print(r.size); r;\n");
  Result r = go(sc, 0);
  EXPECT_EQ(r.out.raw, "1,10,4");
  EXPECT_EQ(r.out.result, "[1, 2, 5, 10]");
  EXPECT_EQ(r.pending_depth, 0u);
  EXPECT_EQ(r.nested, 0u) << "an instruction site never runs nested";
  EXPECT_EQ(r.suspended, 4u) << "one request per element";
  Result j = go(sc, 1);
  EXPECT_EQ(j.out.raw, r.out.raw);
  EXPECT_EQ(j.out.result, r.out.result);
  EXPECT_EQ(j.out.errors, r.out.errors);
}

TEST(Reentry, MapOfNothingAnswersAnEmptyArrayWithoutAsking) {
  Result r = go(scenario("use t; function sq(x) { return x; } t.mapn(0, sq);\n"), 0);
  EXPECT_EQ(r.out.result, "[]");
  EXPECT_EQ(r.suspended, 0u);
}

TEST(Reentry, TheFunctionsErrorValueIsTheContinuationsToReturn) {
  Result r = go(scenario("use t; function bad(x) { return 1 / 0; } r = t.mapn(3, bad); r;\n"), 0);
  EXPECT_NE(r.out.result.find("Divide by zero"), std::string::npos) << r.out.result;
  EXPECT_EQ(r.pending_depth, 0u);
}

TEST(Reentry, SortAsksOnceForEachComparisonAndSorts) {
  Scenario sc = scenario("use t; function lt(a, b) { return a < b; } function gt(a, b) { return a > b; }\n"
                         "x = t.sortc([5, 2, 9, 1, 7], lt); y = t.sortc([5, 2, 9, 1, 7], gt);\n"
                         "print(x[0]); print(x[4]); print(y[0]); print(y[4]); x[2];\n");
  Result r = go(sc, 0);
  EXPECT_EQ(r.out.raw, "1991");
  EXPECT_EQ(r.out.result, "5");
  // Selection of five elements: 4 + 3 + 2 + 1 comparisons, each a request, twice.
  EXPECT_EQ(r.suspended, 20u);
  EXPECT_EQ(r.nested, 0u);
  Result j = go(sc, 1);
  EXPECT_EQ(j.out.raw, r.out.raw);
}

TEST(Reentry, TheComparatorsErrorIsSeenAsAValueAndMayBeReturned) {
  Result r = go(scenario("use t; function bad(a, b) { return a / 0; } x = t.sortc([3, 1, 2], bad); x;\n"), 0);
  EXPECT_NE(r.out.result.find("Divide by zero"), std::string::npos) << r.out.result;
}

TEST(Reentry, EachRunsTheFunctionForEachElementInOrderWithTheArgumentsOfTheEntry) {
  Result r = go(scenario("use t; function p(x) { print(x); print(\";\"); return 0; } t.each([4, 5, 6], p); print(\"end\");\n"), 0);
  EXPECT_EQ(r.out.raw, "4;5;6;end");
  EXPECT_EQ(r.suspended, 3u);
}

TEST(Reentry, ACalleeThatCannotBeEnteredIsTheCalleesValueNotAFailureOfTheRequest) {
  // Wrong arity: the error value is the callee's result, which mapn's continuation returns.
  Result r = go(scenario("use t; function two(a, b) { return 1; } x = t.mapn(2, two); x;\n"), 0);
  EXPECT_NE(r.out.result.find("Argument Count Mismatch"), std::string::npos) << r.out.result;
  EXPECT_EQ(r.pending_depth, 0u);
}

TEST(Reentry, TheCalleeKindsAnyCallAccepts) {
  // A template, a non-resumable native, a resumable native (which chains) and a callable instance.
  const std::string source = std::string(kPrelude) +
      "use t; use tpl; use inc; use resumable; v = t.view(tpl);\n"
      "function g() { return 5; }\n"
      "a = t.mapn(1, inc); print(a[0]); print(\"|\");\n"          // a native, called with 0
      "b = t.mapn(1, tpl); print(b);\n";                          // a template with an argument: argument count mismatch
  Scenario sc = scenario(source);
  Compiled tplsrc("print(\"T\");", tt::Mode::Script, "tpl.tang");
  ASSERT_TRUE(tplsrc.ok());
  sc.before = [&](Context & c) {
    EXPECT_EQ(gltang_library_add_template(c.library(), "tpl", tplsrc.program, 100000, GLTANG_SCOPE_EMPTY), GLTANG_OK);
  };
  Result r = go(sc, 0);
  EXPECT_EQ(r.out.raw.substr(0, 2), "1|") << r.out.raw;
}

TEST(Reentry, AHostAttributeOfAStringAnswersABoundResumableNativeThatCallsGuestCode) {
  Scenario sc = scenario("function f(x) { return x + \"!\"; } s = \"hi\"; r = s.apply(f); print(r); print(\"|\"); print(\"yo\".apply(f)); r;\n");
  Result r = go(sc, 0);
  EXPECT_EQ(r.out.raw, "hi!|yo!");
  EXPECT_EQ(r.out.result, "hi!");
  EXPECT_EQ(r.suspended, 2u);
  EXPECT_EQ(r.pending_depth, 0u);
  Result j = go(sc, 1);
  EXPECT_EQ(j.out.raw, r.out.raw);
  Scenario t = sc;
  t.torture = 1;
  t.verify = 1;
  t.moving = 1;
  EXPECT_EQ(go(t, 0).out.raw, r.out.raw);
}

TEST(Reentry, ACalleeThatItselfAsksChainsThroughAResumableNativeAndThroughACallableObject) {
  Compiled tplsrc("print(\"<i>\"); print(\"x\"); print(\"</i>\");", tt::Mode::Script, "tpl.tang");
  ASSERT_TRUE(tplsrc.ok());
  Scenario sc = scenario("use t; use tpl; function sq(x) { return x * x; } b = t.bind(sq); v = t.view(tpl);\n"
                         "r = t.mapn(3, b); q = t.mapn(2, v); print(r[0]); print(\",\"); print(r[1]); print(\",\"); print(r[2]); print(\"|\"); print(q);\n");
  sc.before = [&](Context & c) {
    EXPECT_EQ(gltang_library_add_template(c.library(), "tpl", tplsrc.program, 100000, GLTANG_SCOPE_EMPTY), GLTANG_OK);
  };
  Result r = go(sc, 0);
  EXPECT_EQ(r.out.raw, "1000,1001,1004|<i>x</i>") << "mapn ends on a callee value that is not an integer, and answers it";
  EXPECT_EQ(r.pending_depth, 0u);
  // Three requests of mapn's, each chaining the bound native's own; one of mapn's, chaining the view's.
  EXPECT_EQ(r.suspended, 3u + 3u + 1u + 1u);
  EXPECT_EQ(r.nested, 0u);
  Result j = go(sc, 1);
  EXPECT_EQ(j.out.raw, r.out.raw);
  Scenario t = sc;
  t.torture = 1;
  t.verify = 1;
  t.moving = 1;
  EXPECT_EQ(go(t, 0).out.raw, r.out.raw);
  // Pausing anywhere in the chain of the bound native gives the same output. (A pause at a template's entry poll is a
  // pause inside the template's own budget scope, which the harness's raising of the total fuel does not satisfy; the
  // template part of this program is left out of the sweep, as it is for an ordinary template call.)
  Scenario chain = scenario("use t; function sq(x) { return x * x; } b = t.bind(sq); r = t.mapn(4, b); print(r[1]); print(\",\"); print(r[3]);\n");
  Result whole = go(chain, 0);
  EXPECT_EQ(whole.out.raw, "1001,1009");
  for (uint64_t step : {2u, 3u, 5u, 7u, 11u, 29u}) {
    for (long threshold : {0l, 1l}) {
      Scenario p = chain;
      p.fuel = step;
      p.step = step;
      Result paused = go(p, threshold);
      EXPECT_EQ(paused.out.raw, whole.out.raw) << "step " << step;
      EXPECT_EQ(paused.out.fuel, whole.out.fuel) << "step " << step;
      EXPECT_GE(paused.out.pauses.size(), 1u);
    }
  }
}

TEST(Reentry, ACallableObjectRunsATemplateAndAnswersItsOutput) {
  Compiled tplsrc("print(\"<b>\"); print(\"hi\"); print(\"</b>\");", tt::Mode::Script, "tpl.tang");
  ASSERT_TRUE(tplsrc.ok());
  Scenario sc = scenario("use t; use tpl; v = t.view(tpl); a = v(); b = v(1, 2); print(a); print(\"/\"); print(b);\n");
  sc.before = [&](Context & c) {
    EXPECT_EQ(gltang_library_add_template(c.library(), "tpl", tplsrc.program, 100000, GLTANG_SCOPE_EMPTY), GLTANG_OK);
  };
  Result r = go(sc, 0);
  EXPECT_EQ(r.out.raw, "<b>hi</b>/<b>hi</b>");
  EXPECT_EQ(r.suspended, 2u);
  Result j = go(sc, 1);
  EXPECT_EQ(j.out.raw, r.out.raw);
}

TEST(Reentry, AnIteratorThatComputesEachItemWithATangFunctionYieldsThemAndEndsOnDone) {
  Scenario sc = scenario("use t; function sq(i) { return i * i + 10; } g = t.gen(sq, 0, 4); for (x : g) { print(x); print(\",\"); } print(\"end\");\n");
  Result r = go(sc, 0);
  EXPECT_EQ(r.out.raw, "10,11,14,19,end");
  EXPECT_EQ(r.suspended, 4u);
  Result j = go(sc, 1);
  EXPECT_EQ(j.out.raw, r.out.raw);
}

TEST(Reentry, ADoneFromTheContinuationOfAnIteratorStepEndsTheLoop) {
  Scenario sc = scenario("use t; function sq(i) { return i * i + 10; } g = t.gen(sq, 2, 5); for (x : g) { print(x); print(\",\"); }\n");
  Result r = go(sc, 0);
  EXPECT_EQ(r.out.raw, "10,11,");
  EXPECT_EQ(r.out.result, "null") << "the loop is the last statement, and ended on DONE, which is no error";
  EXPECT_EQ(r.pending_depth, 0u);
}

TEST(Reentry, AnErrorTheIteratorsFunctionMakesEndsTheLoopAsItsValue) {
  Result r = go(scenario("use t; function bad(i) { if (i == 2) { return 1 / 0; } return i; } g = t.gen(bad, 0, 5); for (x : g) { print(x); } print(\"after\");\n"), 0);
  EXPECT_EQ(r.out.raw, "01after");
}

// ---------------------------------------------------------------------------
// Every eligible operation, against the same operation answered without a request
// ---------------------------------------------------------------------------

namespace {

struct OpProgram {
  const char * name;
  const char * expr;  ///< over x (the instance); ends in the value the program's result shows
};

const OpProgram kOps[] = {
  {"add", "x + 1"}, {"add reflected", "1 + x"}, {"subtract", "x - 1"}, {"multiply", "x * 2"}, {"divide", "x / 2"}, {"modulo", "x % 2"},
  {"negate", "-x"}, {"less", "x < 1"}, {"less_equal", "x <= 1"}, {"greater", "x > 1"}, {"greater_equal", "x >= 1"},
  {"equal", "x == 1"}, {"not_equal", "x != 1"}, {"attribute_get", "x.name"}, {"attribute_set", "x.name = 9"},
  {"index_get", "x[3]"}, {"index_set", "x[3] = 9"}, {"slice", "x[1:2:3]"}, {"call", "x(1, 2)"},
  {"cast int", "x as int"}, {"cast float", "x as float"}, {"cast string", "x as string"},
  {"iterator", "r = 0; for (e : x) { r = r + e; } r"}, {"print", "print(x); 1"}, {"compound add", "x += 1; x"},
};

}  // namespace

TEST(Reentry, EveryEligibleOperationGivesTheSameAnswerAsTheOperationAnsweredWithoutARequest) {
  size_t compared = 0;
  for (const OpProgram & p : kOps) {
    for (int mode = 0; mode <= 5; ++mode) {
      const std::string request = std::string(kPrelude) + "x = ops(f, " + std::to_string(mode) + "); " + p.expr + ";\n";
      const std::string direct = std::string(kPrelude) + "x = ref(f, " + std::to_string(mode) + "); " + p.expr + ";\n";
      Result a = go(scenario(request), 0);
      Result b = go(scenario(direct), 0);
      EXPECT_EQ(a.out.raw, b.out.raw) << p.name << " mode " << mode;
      EXPECT_EQ(a.out.result, b.out.result) << p.name << " mode " << mode;
      EXPECT_EQ(a.out.errors, b.out.errors) << p.name << " mode " << mode;
      EXPECT_EQ(a.pending_depth, 0u) << p.name << " mode " << mode;
      EXPECT_EQ(a.nested, 0u) << p.name << " mode " << mode << ": an instruction site runs nothing nested";
      EXPECT_EQ(a.out.native_depth, 0u);
      if (b.suspended != 0u) {
        ADD_FAILURE() << "the reference asked";
      }
      // The JIT differential: compiled code exits to the interpreter for the custom operand, so the answers agree.
      Result j = go(scenario(request), 1);
      EXPECT_EQ(j.out.raw, a.out.raw) << p.name << " mode " << mode << " (compiled)";
      EXPECT_EQ(j.out.result, a.out.result) << p.name << " mode " << mode << " (compiled)";
      EXPECT_EQ(j.out.errors, a.out.errors) << p.name << " mode " << mode << " (compiled)";
      ++compared;
    }
  }
  EXPECT_EQ(compared, sizeof(kOps) / sizeof(kOps[0]) * 6u);
}

TEST(Reentry, ARequestAsksOncePerStepAndTheStepsBeforeAreNotRepeated) {
  // Left asks and declines; the right asks and answers: the reflected side is step 1, and the left's callback runs once.
  Scenario sc = scenario(std::string(kPrelude) + "use t; function f2() { return 2; } a = t.rx(f2, 1, 1); b = t.rx(f2, 0, 2); r = a + b; r;\n");
  Result r = go(sc, 0);
  EXPECT_EQ(r.out.result, "2007");
  const std::vector<std::string> want = {"add left 1 entry step 0", "cont 1 step 0 result 2", "add right 2 entry step 0", "cont 2 step 1 result 2"};
  EXPECT_EQ(r.log, want);
  EXPECT_EQ(r.pending_depth, 0u);
}

TEST(Reentry, ANotEqualChainRunsTheFourStepsInOrderEachOnce) {
  // not_equal of both sides declines (one after asking), then equal of both: steps 0 to 3.
  Scenario sc = scenario(std::string(kPrelude) + "use t; function f2() { return 2; } a = t.rx(f2, 1, 1); b = t.rx(f2, 1, 2); r = a != b; r;\n");
  Result r = go(sc, 0);
  // `!=` falls to `==`: both decline again, so the identity rule answers: different objects, so not equal.
  EXPECT_EQ(r.out.result, "true");
  const std::vector<std::string> want = {
    "not_equal left 1 entry step 0", "cont 1 step 0 result 2",
    "not_equal right 2 entry step 0", "cont 2 step 1 result 2",
    "equal left 1 entry step 0", "cont 1 step 2 result 2",
    "equal right 2 entry step 0", "cont 2 step 3 result 2"};
  EXPECT_EQ(r.log, want);
  EXPECT_EQ(r.pending_depth, 0u);
}

TEST(Reentry, ARequestFromTheRightOnlyAndAPrintThenToStringFallThroughTheirSteps) {
  Scenario sc = scenario(std::string(kPrelude) + "use t; function f2() { return 2; } a = t.rx(f2, 2, 1); b = t.rx(f2, 3, 2); r = a + b; r;\n");
  Result r = go(sc, 0);
  // The left declines at once (no request); the right asks twice (a chain through the same operation) and answers.
  EXPECT_EQ(r.out.result, "2007");
  const std::vector<std::string> want = {"add left 1 entry step 0", "add right 2 entry step 0", "cont 2 step 1 result 2", "cont 2 step 1 result 2"};
  EXPECT_EQ(r.log, want);
  EXPECT_EQ(r.pending_depth, 0u) << "each record of the chain was let go when its continuation was called";
}

TEST(Reentry, PrintAsksPrintThenToStringWhenPrintDeclines) {
  // rxtext has both; the instance's plan 1 makes `print` decline after asking, so `to_string` is step 1 and its text is printed.
  Scenario sc = scenario(std::string(kPrelude) + "use t; function f2() { return 2; } a = t.rxtext(f2, 1, 1); print(a); print(\"|\"); s = a as string; print(s);\n");
  Result r = go(sc, 0);
  // With plan 1 every entry's continuation declines: print falls to to_string, whose continuation declines too: nothing is printed.
  EXPECT_EQ(r.out.raw, "|");
  EXPECT_EQ(r.pending_depth, 0u);
  const std::vector<std::string> want = {"print 1 entry step 0", "cont 1 step 0", "to_string 1 entry step 0", "cont 1 step 1",
                                        "to_string 1 entry step 0", "cont 1 step 1"};
  // `as string` is not a print: it asks to_string alone (cast is absent from rxtext).
  EXPECT_EQ(r.log, want);
}

TEST(Reentry, TheTextOfAPlanZeroInstanceIsPrintedWithItsTags) {
  Scenario sc = scenario(std::string(kPrelude) + "use t; function f2() { return 2; } a = t.rxtext(f2, 0, 3); print(a); print(\"|\"); s = a as string; print(s);\n");
  Result r = go(sc, 0);
  EXPECT_EQ(r.out.raw, "3009|3009");
}

TEST(Reentry, ACallbackThatMakesNoRequestAllocatesWhatItDidBeforeWhetherOrNotItsTypeIsResumable) {
  // Answers, declines, errors and a continuation-free path: the heap sees the same allocations and the run the same fuel.
  for (int mode : {0, 1, 2, 4}) {
    const std::string body = "x = %s(f, " + std::to_string(mode) + "); n = 0; for (i = 0; i < 40; i += 1) { y = x + i; n += 1; } z = -x; n;\n";
    std::string a = std::string(kPrelude) + "use t; refr = t.refr; " + body;
    std::string b = a;
    a.replace(a.find("%s"), 2, "ref");
    b.replace(b.find("%s"), 2, "refr");
    Result plain = go(scenario(a), 0);
    Result resumable = go(scenario(b), 0);
    EXPECT_EQ(plain.out.heap_allocations, resumable.out.heap_allocations) << "mode " << mode;
    EXPECT_EQ(plain.out.fuel, resumable.out.fuel) << "mode " << mode;
    EXPECT_EQ(plain.out.result, resumable.out.result);
    EXPECT_EQ(resumable.suspended, 0u);
  }
}

TEST(Reentry, AnAssignmentOfNoAnswerIsWorthTheValueAssignedAfterTheCallee) {
  Result a = go(scenario(std::string(kPrelude) + "x = ops(f, 5); r = (x.name = 9); r;\n"), 0);
  Result b = go(scenario(std::string(kPrelude) + "x = ref(f, 5); r = (x.name = 9); r;\n"), 0);
  EXPECT_EQ(a.out.result, "9");
  EXPECT_EQ(a.out.result, b.out.result);
}

// ---------------------------------------------------------------------------
// What a suspension holds, and what it does not
// ---------------------------------------------------------------------------

TEST(Reentry, NoScopeNativeDepthUnitOrRecordOfTheCallbackIsLiveWhileTheCalleeRuns) {
  // The callee reads the native-depth units, the activation records and the open handle scopes; they are what they were
  // before the callback asked.
  Scenario sc = scenario("use t; use depth; function probe(x) { use depth; use records; use scopes; print(depth()); print(\",\"); print(records()); print(\",\"); print(scopes()); print(\";\"); return x; }\n"
                         "use records; use scopes; print(depth()); print(\",\"); print(records()); print(\",\"); print(scopes()); print(\";\");\n"
                         "r = t.mapn(2, probe); print(r.size);\n");
  Result r = go(sc, 0);
  // before ; inside (twice) ; the size
  const std::string before = r.out.raw.substr(0, r.out.raw.find(';') + 1);
  EXPECT_EQ(r.out.raw, before + before + before + "2");
}

TEST(Reentry, NoScopeNativeDepthUnitOrRecordOfAnOperationsCallbackIsLiveWhileTheCalleeRuns) {
  Scenario sc = scenario("use t; use depth; use records; use scopes;\n"
                         "function probe() { use depth; use records; use scopes; print(depth()); print(\",\"); print(records()); print(\",\"); print(scopes()); print(\";\"); return 41; }\n"
                         "x = t.ops(probe, 0); print(depth()); print(\",\"); print(records()); print(\",\"); print(scopes()); print(\";\");\n"
                         "y = x + 1; print(y);\n");
  for (long threshold : {0l, 1l}) {
    Result r = go(sc, threshold);
    const std::string before = r.out.raw.substr(0, r.out.raw.find(';') + 1);
    EXPECT_EQ(r.out.raw, before + before + "42") << "threshold " << threshold;
    EXPECT_EQ(r.suspended, 1u);
  }
}

TEST(Reentry, TheCalleeSeesTheCollectorMoveObjectsAndTheStateAndResultSurvive) {
  for (int mode = 0; mode < 2; ++mode) {
    Scenario sc = scenario("use t; function sq(x) { return x * 3; } r = t.mapn(6, sq); r;\n");
    sc.torture = 1;
    sc.verify = 1;
    sc.moving = 1;
    sc.on_poll = [](Script &, GRCORE_Context * context, uint64_t) { EXPECT_EQ(grheap_collect(grheap_heap_get(context)), GRHEAP_OK); };
    Result r = go(sc, mode);
    EXPECT_EQ(r.out.result, "[0, 3, 6, 9, 12, 15]") << "threshold " << mode;
  }
}

TEST(Reentry, TheCollectionNativesSurviveTortureWithManyRequests) {
  Scenario sc = scenario("use t; function lt(a, b) { return a < b; } x = t.sortc([9, 4, 7, 1, 8, 2, 6], lt); x;\n");
  sc.torture = 1;
  sc.verify = 1;
  sc.moving = 1;
  Result r = go(sc, 0);
  EXPECT_EQ(r.out.result, "[1, 2, 4, 6, 7, 8, 9]");
}

// ---------------------------------------------------------------------------
// Misuse
// ---------------------------------------------------------------------------

namespace {

bool misuse_native(GLTANG_NativeCall * call, void * user);

GLTANG_Handle g_stale = {0, 0};
int g_refused = 0;  ///< The misuse case whose request gltang_call_guest refused (GLTANG_ERR_INVALID), or 0.

GLTANG_OpStatus misuse_cont(GLTANG_NativeCall * call, void *, GLTANG_Handle state, GLTANG_Handle) {
  // A handle of the entry's scope is not valid in this one; a state that is an array carries what the entry learned.
  if (g_stale.scope != 0 && gltang_handle_valid(call, g_stale)) {
    gltang_call_return_integer(call, 99);
    return GLTANG_OP_OK;
  }
  GLTANG_Handle x;
  if (gltang_handle_kind(call, state) == GLTANG_KIND_ARRAY && gltang_handle_array_get(call, state, 0, &x) == GLTANG_OK) {
    gltang_call_return_integer(call, gltang_handle_integer(call, x));
    return GLTANG_OP_OK;
  }
  gltang_call_return_integer(call, 77);
  return GLTANG_OP_OK;
}

const GLTANG_Continuation kMisuseTable[] = {{"misuse_cont", misuse_cont}};

/// Each case of the misuse native: what it does, by its first argument.
///   0 PENDING with no request        1 a request and then OK            2 two requests            3 a bad continuation index
///   4 a stale handle (an old scope)  5 a callee that is not callable    6 too many arguments      7 a good request (control)
///   8 a request from a native that is not marked resumable (see misuse_plain)
bool misuse_native(GLTANG_NativeCall * call, void *) {
  GLTANG_Handle f, zero = {0, 0};
  const int64_t mode = gltang_call_integer(call, 0);
  if (gltang_call_arg(call, 1, &f) != GLTANG_OK) {
    return false;
  }
  GLTANG_Result r = GLTANG_OK;
  switch (mode) {
    case 1:
      r = gltang_call_guest(call, f, nullptr, 0, 0, zero);
      gltang_call_return_integer(call, r == GLTANG_OK ? 1 : 0);
      return false;  // a request, and then a failure: the request is dropped
    case 2:
      // The second request is refused, which fails the call: the first runs nothing either.
      r = gltang_call_guest(call, f, nullptr, 0, 0, zero);
      return gltang_call_guest(call, f, nullptr, 0, 0, zero) == GLTANG_OK && r == GLTANG_OK;
    case 10:
      g_stale = f;
      return gltang_call_guest(call, f, nullptr, 0, 0, zero) == GLTANG_OK;
    case 3:
      r = gltang_call_guest(call, f, nullptr, 0, 1, zero);  // the table has one entry: index 1 is one past it
      g_refused = r == GLTANG_ERR_INVALID ? 3 : 0;
      gltang_call_return_integer(call, 3);  // an answer after the refusal does not clear it
      return true;
    case 9:
      g_stale = f;  // a handle of this call, stale in the next
      return true;
    case 4:
      r = gltang_call_guest(call, f, nullptr, 0, 0, g_stale);
      g_refused = r == GLTANG_ERR_INVALID ? 4 : 0;
      gltang_call_return_integer(call, 4);
      return true;
    case 5: {
      GLTANG_Handle n;
      r = gltang_handle_make_integer(call, 5, &n) == GLTANG_OK ? gltang_call_guest(call, n, nullptr, 0, 0, zero) : GLTANG_OK;
      g_refused = r == GLTANG_ERR_INVALID ? 5 : 0;
      gltang_call_return_integer(call, 5);
      return true;
    }
    case 6: {
      GLTANG_Handle args[GLTANG_GUEST_CALL_ARGS_MAX + 1u];
      for (auto & a : args) {
        a = f;
      }
      r = gltang_call_guest(call, f, args, GLTANG_GUEST_CALL_ARGS_MAX + 1u, 0, zero);
      g_refused = r == GLTANG_ERR_INVALID ? 6 : 0;
      gltang_call_return_integer(call, 6);
      return true;
    }
    case 7:
      return gltang_call_guest(call, f, nullptr, 0, 0, zero) == GLTANG_OK;
    default:
      return true;
  }
}

bool misuse_plain(GLTANG_NativeCall * call, void *) {
  GLTANG_Handle f, zero = {0, 0};
  if (gltang_call_arg(call, 0, &f) != GLTANG_OK) {
    return false;
  }
  GLTANG_Result r = gltang_call_guest(call, f, nullptr, 0, 0, zero);
  g_refused = r == GLTANG_ERR_INVALID ? 8 : 0;
  gltang_call_return_integer(call, 8);
  return true;
}

GLTANG_OpStatus pending_without_request(GLTANG_NativeCall *, void *, GLTANG_Handle) { return GLTANG_OP_PENDING; }
GLTANG_OpStatus request_then_ok(GLTANG_NativeCall * call, void *, GLTANG_Handle self) {
  GLTANG_Handle f;
  GLTANG_Result r = gltang_handle_custom_slot_get(call, self, 0, &f);
  if (r == GLTANG_OK) {
    r = gltang_call_guest(call, f, nullptr, 0, 0, GLTANG_Handle{0, 0});
  }
  gltang_call_return_integer(call, 5);
  return GLTANG_OP_OK;  // the request is dropped
}
GLTANG_OpStatus opaque_request(GLTANG_NativeCall * call, void *, GLTANG_Handle self) {
  GLTANG_Handle f;
  if (gltang_handle_custom_slot_get(call, self, 0, &f) != GLTANG_OK) {
    return GLTANG_OP_ERROR;
  }
  const GLTANG_Result r = gltang_call_guest(call, f, nullptr, 0, 0, GLTANG_Handle{0, 0});
  g_refused = r == GLTANG_ERR_INVALID ? 11 : 0;
  gltang_call_return_integer(call, 11);  // an answer after the refusal does not clear it
  return GLTANG_OP_OK;
}
GLTANG_OpStatus declining_native_cont(GLTANG_NativeCall *, void *, GLTANG_Handle, GLTANG_Handle) { return GLTANG_OP_DECLINE; }
GLTANG_OpStatus done_native_cont(GLTANG_NativeCall *, void *, GLTANG_Handle, GLTANG_Handle) { return GLTANG_OP_DONE; }
GLTANG_OpStatus pending_native_cont(GLTANG_NativeCall *, void *, GLTANG_Handle, GLTANG_Handle) { return GLTANG_OP_PENDING; }
bool ask_for_cont(GLTANG_NativeCall * call, void *) {
  GLTANG_Handle f, zero = {0, 0};
  return gltang_call_arg(call, 0, &f) == GLTANG_OK && gltang_call_guest(call, f, nullptr, 0, 0, zero) == GLTANG_OK;
}
const GLTANG_Continuation kDecliningTable[] = {{"c", declining_native_cont}};
const GLTANG_Continuation kDoneTable[] = {{"c", done_native_cont}};
const GLTANG_Continuation kPendingTable[] = {{"c", pending_native_cont}};

}  // namespace

TEST(ReentryMisuse, EveryRefusalIsHostFunctionFailedOrAnInvalidResultAndNothingRunsOrLeaks) {
  for (int mode = 1; mode <= 10; ++mode) {
    if (mode == 9) {
      continue;  // the helper of mode 4
    }
    g_stale = GLTANG_Handle{0, 0};
    g_refused = 0;
    Types types;
    uint64_t pending = 99;
    // After the call, the handle scopes open (none: the refused call's is closed) are printed after the bar.
    Scenario sc = scenario("use t; use scopes; function f() { print(\"RAN\"); return 1; } " + std::string(mode == 4 ? "u.misuse(9, f); " : "") + "r = u.misuse(" + std::to_string(mode) + ", f); print(\"|\"); print(scopes()); r;\n");
    sc.after = [&](Context & c) { pending = gltang_vm_test_pending_depth(c.execution); };
    sc.before = [&](Context & c) {
      install(c, types);
      GLTANG_Library * lib = nullptr;
      EXPECT_EQ(gltang_library_create("u", &lib), GLTANG_OK);
      EXPECT_EQ(gltang_library_add_native(lib, "misuse", misuse_native, nullptr), GLTANG_OK);
      EXPECT_EQ(gltang_library_mark_resumable(lib, "misuse", kMisuseTable, 1), GLTANG_OK);
      EXPECT_EQ(gltang_library_add_native(lib, "plain", misuse_plain, nullptr), GLTANG_OK);
      EXPECT_EQ(gltang_library_add_library(c.library(), lib), GLTANG_OK);
      gltang_library_release(lib);
    };
    if (mode == 8) {
      sc.source = "use t; use u; use scopes; function f() { print(\"RAN\"); return 1; } r = u.plain(f); print(\"|\"); print(scopes()); r;\n";
    }
    sc.source = "use u; " + sc.source;
    Outcome o = run(sc, 0);
    switch (mode) {
      case 1:
      case 2:
        // A request followed by a failure, and a second request, which the engine refuses and which fails the call: Host function failed, and the function never ran.
        EXPECT_EQ(o.raw, "|0") << "mode " << mode;
        EXPECT_NE(o.result.find("Host function failed"), std::string::npos) << o.result;
        break;
      case 10:
        EXPECT_NE(o.raw.find("RAN"), std::string::npos);
        EXPECT_EQ(o.result, "77") << "a handle of the entry is stale in the continuation";
        break;
      case 7:
        EXPECT_NE(o.raw.find("RAN"), std::string::npos) << "the control asks for a good call and gets it";
        break;
      default:
        // Refused, and the refusal stands: the integer the native answered after it is not the call's value.
        EXPECT_EQ(o.raw, "|0") << "mode " << mode << " ran the function, or left a scope open";
        EXPECT_EQ(g_refused, mode) << "mode " << mode << ": gltang_call_guest answered GLTANG_ERR_INVALID";
        EXPECT_NE(o.result.find("Host function failed"), std::string::npos) << "mode " << mode << ": " << o.result;
        break;
    }
    EXPECT_EQ(o.native_depth, 0u) << "mode " << mode;
    EXPECT_EQ(pending, 0u) << "mode " << mode;
  }
}

TEST(ReentryMisuse, PendingWithNoRequestARequestThenOkAndARequestFromAnOpaqueCallbackAreHostFunctionFailed) {
  GLTANG_TypeRegistry * registry = nullptr;
  ASSERT_EQ(gltang_type_registry_create(&registry), GLTANG_OK);
  const GLTANG_Type * a = nullptr;
  const GLTANG_Type * b = nullptr;
  const GLTANG_Type * c = nullptr;
  GLTANG_TypeDescriptor d = GLTANG_TYPE_DESCRIPTOR_INIT;
  d.name = "nothing";
  d.thread_safe = true;
  d.slot_count = 1;
  d.negate = pending_without_request;
  d.resumable_ops = 1u << GLTANG_TYPE_OP_NEGATE;
  d.continuations = kMisuseTable;
  d.continuation_count = 1;
  ASSERT_EQ(gltang_type_registry_add(registry, &d, &a), GLTANG_OK);
  d = GLTANG_TYPE_DESCRIPTOR_INIT;
  d.name = "thenok";
  d.thread_safe = true;
  d.slot_count = 1;
  d.negate = request_then_ok;
  d.resumable_ops = 1u << GLTANG_TYPE_OP_NEGATE;
  d.continuations = kMisuseTable;
  d.continuation_count = 1;
  ASSERT_EQ(gltang_type_registry_add(registry, &d, &b), GLTANG_OK);
  d = GLTANG_TYPE_DESCRIPTOR_INIT;
  d.name = "opaque";
  d.thread_safe = true;
  d.slot_count = 1;
  d.negate = opaque_request;
  // It has a table and lists another operation: the mask, not the table, is what makes `negate` opaque.
  static Fx unused;
  d.user = &unused;
  d.add = ops_add;
  d.resumable_ops = 1u << GLTANG_TYPE_OP_ADD;
  d.continuations = kMisuseTable;
  d.continuation_count = 1;
  ASSERT_EQ(gltang_type_registry_add(registry, &d, &c), GLTANG_OK);
  struct Holder {
    const GLTANG_Type * type;
  };
  static Holder holders[3];
  holders[0] = {a};
  holders[1] = {b};
  holders[2] = {c};
  for (int k = 0; k < 3; ++k) {
    g_refused = 0;
    uint64_t pending = 99;
    Scenario sc = scenario("use u; use scopes; function f() { print(\"RAN\"); return 1; } x = u.mk(f); y = -x; print(\"|\"); print(scopes()); y;\n");
    sc.after = [&](Context & ctx) { pending = gltang_vm_test_pending_depth(ctx.execution); };
    sc.before = [&](Context & ctx) {
      ctx.add_native_library();
      GLTANG_Library * lib = nullptr;
      EXPECT_EQ(gltang_library_create("u", &lib), GLTANG_OK);
      EXPECT_EQ(gltang_library_add_native(lib, "mk", [](GLTANG_NativeCall * call, void * user) {
        GLTANG_Handle h, f;
        if (gltang_handle_make_custom(call, static_cast<Holder *>(user)->type, &h) != GLTANG_OK || gltang_call_arg(call, 0, &f) != GLTANG_OK || gltang_handle_custom_slot_set(call, h, 0, f) != GLTANG_OK) {
          return false;
        }
        gltang_call_return_handle(call, h);
        return true;
      }, &holders[k]), GLTANG_OK);
      EXPECT_EQ(gltang_library_add_library(ctx.library(), lib), GLTANG_OK);
      gltang_library_release(lib);
      EXPECT_EQ(gltang_execution_attach_types(ctx.execution, registry), GLTANG_OK);
    };
    Outcome o = run(sc, 0);
    EXPECT_EQ(o.raw, "|0") << "case " << k << ": nothing runs, and no scope is left open";
    EXPECT_NE(o.result.find("Host function failed"), std::string::npos) << "case " << k << ": " << o.result;
    if (k == 2) {
      EXPECT_EQ(g_refused, 11) << "an opaque callback's request is refused, and the answer it gave after does not stand";
    }
    EXPECT_EQ(o.native_depth, 0u);
    EXPECT_EQ(pending, 0u) << "case " << k;
  }
  gltang_type_registry_release(registry);
}

TEST(ReentryMisuse, ANativeContinuationThatDeclinesOrIsDoneOrAsksForNothingIsHostFunctionFailed) {
  struct Case {
    const GLTANG_Continuation * table;
  };
  const Case cases[] = {{kDecliningTable}, {kDoneTable}, {kPendingTable}};
  for (const Case & k : cases) {
    Scenario sc = scenario("use u; function f() { return 1; } r = u.ask(f); r;\n");
    sc.before = [&](Context & ctx) {
      GLTANG_Library * lib = nullptr;
      EXPECT_EQ(gltang_library_create("u", &lib), GLTANG_OK);
      EXPECT_EQ(gltang_library_add_native(lib, "ask", ask_for_cont, nullptr), GLTANG_OK);
      EXPECT_EQ(gltang_library_mark_resumable(lib, "ask", k.table, 1), GLTANG_OK);
      EXPECT_EQ(gltang_library_add_library(ctx.library(), lib), GLTANG_OK);
      gltang_library_release(lib);
    };
    Outcome o = run(sc, 0);
    EXPECT_NE(o.result.find("Host function failed"), std::string::npos) << o.result;
  }
}

TEST(Reentry, ATemplateCalleeWhoseOwnBudgetRunsOutIsTheLimitErrorAsTheCalleesValue) {
  Compiled spin("k = 0; while (true) { k = k + 1; }", tt::Mode::Script, "spin.tang");
  ASSERT_TRUE(spin.ok());
  Scenario sc = scenario("use t; use spin; v = t.view(spin); a = v(); print(\"after\"); a;\n");
  sc.before = [&](Context & c) {
    EXPECT_EQ(gltang_library_add_template(c.library(), "spin", spin.program, 60, GLTANG_SCOPE_EMPTY), GLTANG_OK);
  };
  for (long threshold : {0l, 1l}) {
    Result r = go(sc, threshold);
    EXPECT_EQ(r.out.raw, "after");
    EXPECT_NE(r.out.result.find("Limit"), std::string::npos) << r.out.result;
    EXPECT_EQ(r.pending_depth, 0u);
    EXPECT_TRUE(r.out.finished);
  }
}

TEST(Reentry, TheEntryTheRequestAndTheContinuationEachCostWhatTheyShould) {
  // Golden totals of three small runs: the instructions, one native-call charge for each callback entered
  // (the entry and every continuation) and one for each request. A callback or a request that is not charged
  // changes them; the pause sweep compares a run with itself and cannot see that.
  Result map = go(scenario("use t; function id(x) { return x; } r = t.mapn(3, id); 0;\n"), 0);
  Result add = go(scenario(std::string(kPrelude) + "x = ops(f, 0); y = x + 1; 0;\n"), 0);
  Result chain = go(scenario(std::string(kPrelude) + "x = ops(f, 4); y = x + 1; 0;\n"), 0);
  EXPECT_EQ(map.out.fuel, kFuelMapThree);
  EXPECT_EQ(add.out.fuel, kFuelAdd);
  EXPECT_EQ(chain.out.fuel - add.out.fuel, kFuelChainExtra) << "a second request: one more callee, one more continuation";
}

// ---------------------------------------------------------------------------
// Embedded sites: a C loop with no instruction to run again
// ---------------------------------------------------------------------------

TEST(ReentryEmbedded, ARequestFromInsideAContainerRunsNestedAndTheResultIsCorrect) {
  Scenario sc = scenario("use t; function f() { return 41; } function g() { return 40; } a = t.emb(f); b = t.emb(f); c = t.emb(g);\n"
                         "print([a] == [b]); print(\"|\"); print([c] == [a]); print(\"|\"); print([a]); print(\"|\"); print(([a] as string));\n");
  Result r = go(sc, 0);
  EXPECT_EQ(r.out.raw, "true|false|[42]|[42]");
  EXPECT_GE(r.nested, 4u) << "the element comparisons and the element text each ran a request nested";
  EXPECT_EQ(r.suspended, 0u);
  EXPECT_EQ(r.pending_depth, 0u);
  Result j = go(sc, 1);
  EXPECT_EQ(j.out.raw, r.out.raw);
}

TEST(ReentryEmbedded, TheSameTypeAtAnInstructionSiteSuspendsAndNothingRunsNested) {
  Scenario sc = scenario("use t; function f() { return 41; } a = t.emb(f); b = t.emb(f); print(a == b); print(\"|\"); print(a);\n");
  Result r = go(sc, 0);
  EXPECT_EQ(r.out.raw, "true|42");
  EXPECT_EQ(r.nested, 0u);
  EXPECT_EQ(r.suspended, 2u);
}

TEST(ReentryEmbedded, ANonFunctionCalleeAtAnEmbeddedSiteIsHostFunctionFailed) {
  Scenario sc = scenario("use t; use inc; a = t.emb(inc); b = t.emb(inc); r = [a] == [b]; r;\n");
  Result r = go(sc, 0);
  EXPECT_NE(r.out.result.find("Host function failed"), std::string::npos) << r.out.result;
  EXPECT_EQ(r.nested, 1u);
}

TEST(ReentryEmbedded, APauseInsideANestedRequestIsAnUnwindThatStopsAtTheRecord) {
  Scenario sc = scenario("use t; function f() { k = 0; while (k < 40) { k = k + 1; } return 41; } a = t.emb(f); b = t.emb(f); r = [a] == [b]; print(\"after\"); r;\n");
  sc.fuel = 30;
  sc.step = 30;
  Result r = go(sc, 0);
  EXPECT_EQ(r.out.raw.find("after"), std::string::npos) << "the run unwound";
  EXPECT_EQ(r.pending_depth, 0u);
}

// ---------------------------------------------------------------------------
// Pause, unwind, resume
// ---------------------------------------------------------------------------

namespace {

Scenario pausing(const std::string & source, uint64_t step) {
  Scenario sc = scenario(source);
  sc.fuel = step;
  sc.step = step;
  return sc;
}

const char * kPausable =
    "use t; use depth; function slow(x) { k = 0; while (k < 6) { k = k + 1; } return x + k; }\n"
    "function lt(a, b) { k = 0; while (k < 3) { k = k + 1; } return a < b; }\n"
    "m = t.mapn(5, slow); s = t.sortc([4, 1, 3, 2], lt); a = t.ops(slow, 0); b = a + 1; c = -a;\n"
    "for (x : t.gen(slow, 0, 3)) { print(x); print(\",\"); }\n"
    "print(m[4]); print(\";\"); print(s[0]); print(s[3]); print(\";\"); print(b); print(\";\"); print(c); print(\";\"); print(depth());\n";

}  // namespace

TEST(ReentryPause, APauseAnywhereInsideACalleeResumesToTheUninterruptedOutputAndTheFuelIsEqual) {
  Result whole = go(scenario(kPausable), 0);
  ASSERT_TRUE(whole.out.finished);
  EXPECT_FALSE(whole.out.raw.empty());
  for (uint64_t step : {3u, 7u, 13u, 40u}) {
    for (long threshold : {0l, 1l}) {
      Result r = go(pausing(kPausable, step), threshold);
      EXPECT_TRUE(r.out.finished) << "step " << step;
      EXPECT_EQ(r.out.raw, whole.out.raw) << "resumed after every " << step << " units of fuel, threshold " << threshold;
      EXPECT_EQ(r.out.result, whole.out.result);
      EXPECT_EQ(r.out.fuel, whole.out.fuel) << "the total fuel is the uninterrupted run's";
      EXPECT_GT(r.out.pauses.size(), 3u);
      EXPECT_EQ(r.pending_depth, 0u);
    }
  }
}

TEST(ReentryPause, AResumeOnAnotherThreadAtEveryPauseGivesTheSameOutput) {
  Result whole = go(scenario(kPausable), 0);
  ASSERT_TRUE(whole.out.finished);
  Types types;
  Compiled page(kPausable);
  ASSERT_TRUE(page.ok());
  Config config;
  config.fuel = 25;
  config.jit_threshold = 0;
  Context context(page.program, config);
  ASSERT_TRUE(context.ok());
  install(context, types);
  ASSERT_FALSE(context.execute());
  ASSERT_TRUE(context.paused());
  int hops = 0;
  bool finished = false;
  while (!finished) {
    ASSERT_EQ(grcore_context_release(context.context), GRCORE_OK);
    GRCORE_Result acquired = GRCORE_ERR_INTERNAL;
    std::thread t([&]() {
      acquired = grcore_context_acquire(context.context);
      if (acquired != GRCORE_OK) {
        return;
      }
      grcore_context_set_fuel(context.context, grcore_context_fuel_used(context.context) + 25);
      finished = context.resume();
      grcore_context_release(context.context);
    });
    t.join();
    ASSERT_EQ(acquired, GRCORE_OK);
    ASSERT_EQ(grcore_context_acquire(context.context), GRCORE_OK);
    ASSERT_LT(++hops, 4000);
  }
  EXPECT_GT(hops, 10);
  EXPECT_EQ(context.raw(), whole.out.raw);
  EXPECT_EQ(context.describe(), whole.out.result);
}

TEST(ReentryPause, ALimitThatUnwindsTheRunInsideTheCalleeNeverCallsTheContinuationAndLeaksNothing) {
  // Terminate at each poll in turn: the run ends, the continuation is called for no request that was unwound, and no
  // record survives.
  for (uint64_t at : {2u, 5u, 9u, 14u, 20u}) {
    Types types;
    Scenario sc = scenario("use t; use note; function slow(x) { note(x); k = 0; while (k < 8) { k = k + 1; } return x; } r = t.mapn(4, slow); print(\"done\"); r;\n");
    sc.on_poll = [at](Script & s, GRCORE_Context *, uint64_t n) {
      if (n == at) {
        s.post(GRCORE_REQUEST_TERMINATE);
      }
    };
    uint64_t collected = 0;
    sc.after = [&](Context & c) {
      EXPECT_EQ(grheap_collect(c.heap), GRHEAP_OK);
      GRHEAP_Stats stats;
      EXPECT_EQ(grheap_stats(c.heap, &stats), GRHEAP_OK);
      collected = stats.collections;
    };
    Result r = go(sc, 0);
    EXPECT_EQ(r.out.raw.find("done"), std::string::npos) << "poll " << at << ": the run ended";
    EXPECT_FALSE(r.out.finished);
    EXPECT_EQ(r.pending_depth, 0u) << "poll " << at;
    EXPECT_EQ(r.out.native_depth, 0u);
    EXPECT_GT(collected, 0u);
  }
}

TEST(ReentryPause, ACalleeAtTheGuestDepthIsTheRecursionLimitErrorForTheContinuation) {
  Scenario sc = scenario("use t; function deep(x) { use t; return t.mapn(1, deep); } r = t.mapn(1, deep); r;\n");
  Result r = go(sc, 0);
  EXPECT_NE(r.out.result.find("Recursion"), std::string::npos) << r.out.result;
  EXPECT_EQ(r.pending_depth, 0u);
}

TEST(ReentryPause, ASnapshotAtAPauseInsideACalleeIsRefusedAndNamesTheType) {
  Types types;
  Compiled page("use t; function slow() { k = 0; while (k < 50) { k = k + 1; } return 41; } a = t.ops(slow, 0); a + 1;\n");
  ASSERT_TRUE(page.ok());
  Config config;
  config.fuel = 20;
  Context context(page.program, config);
  ASSERT_TRUE(context.ok());
  install(context, types);
  ASSERT_FALSE(context.execute());
  ASSERT_TRUE(context.paused());
  GLTANG_Snapshot * snapshot = nullptr;
  EXPECT_EQ(gltang_snapshot_take(context.execution, &snapshot), GLTANG_ERR_UNSUPPORTED);
  EXPECT_EQ(snapshot, nullptr);
  EXPECT_EQ(gltang_snapshot_refused_type(context.execution), types.req);
}

TEST(ReentryPause, ASnapshotAtAPauseInsideAResumableNativesCalleeIsRefusedToo) {
  // No instance of a host-defined type is live here: the pending record alone refuses the image.
  Types types;
  Compiled page("use t; function slow(x) { k = 0; while (k < 50) { k = k + 1; } return 41; } r = t.mapn(2, slow); r;\n");
  ASSERT_TRUE(page.ok());
  Config config;
  config.fuel = 20;
  Context context(page.program, config);
  ASSERT_TRUE(context.ok());
  install(context, types);
  ASSERT_FALSE(context.execute());
  ASSERT_TRUE(context.paused());
  EXPECT_EQ(gltang_vm_test_pending_depth(context.execution), 1u);
  GLTANG_Snapshot * snapshot = nullptr;
  EXPECT_EQ(gltang_snapshot_take(context.execution, &snapshot), GLTANG_ERR_UNSUPPORTED);
  EXPECT_EQ(snapshot, nullptr);
  EXPECT_EQ(gltang_snapshot_refused_type(context.execution), nullptr) << "a native has no type to name";
  // Finishing the run leaves nothing pending, and the next snapshot is not refused for it.
  grcore_context_set_fuel(context.context, GRCORE_UNLIMITED);
  ASSERT_TRUE(context.resume());
  EXPECT_EQ(gltang_vm_test_pending_depth(context.execution), 0u);
}

TEST(ReentryPause, ARunOfAFixtureWithAnOldDescriptorSizeWorksAndNothingIsResumable) {
  // A descriptor sized for the first version has no operation that asks; a request from its callback is refused.
  GLTANG_TypeRegistry * registry = nullptr;
  ASSERT_EQ(gltang_type_registry_create(&registry), GLTANG_OK);
  GLTANG_TypeDescriptor d = GLTANG_TYPE_DESCRIPTOR_INIT;
  d.name = "old";
  d.slot_count = 1;
  d.negate = opaque_request;
  d.resumable_ops = kEligible;  // past the size below: not read
  d.size = offsetof(GLTANG_TypeDescriptor, to_string) + sizeof(GLTANG_TypeToStringFn);
  const GLTANG_Type * type = nullptr;
  ASSERT_EQ(gltang_type_registry_add(registry, &d, &type), GLTANG_OK);
  Compiled page("use u; function f() { print(\"RAN\"); return 1; } x = u.mk(f); y = -x; y;\n");
  ASSERT_TRUE(page.ok());
  Context context(page.program);
  ASSERT_TRUE(context.ok());
  GLTANG_Library * lib = nullptr;
  ASSERT_EQ(gltang_library_create("u", &lib), GLTANG_OK);
  ASSERT_EQ(gltang_library_add_native(lib, "mk", [](GLTANG_NativeCall * call, void * user) {
    GLTANG_Handle h, f;
    if (gltang_handle_make_custom(call, static_cast<const GLTANG_Type *>(user), &h) != GLTANG_OK || gltang_call_arg(call, 0, &f) != GLTANG_OK || gltang_handle_custom_slot_set(call, h, 0, f) != GLTANG_OK) {
      return false;
    }
    gltang_call_return_handle(call, h);
    return true;
  }, const_cast<GLTANG_Type *>(type)), GLTANG_OK);
  ASSERT_EQ(gltang_library_add_library(context.library(), lib), GLTANG_OK);
  gltang_library_release(lib);
  ASSERT_EQ(gltang_execution_attach_types(context.execution, registry), GLTANG_OK);
  gltang_type_registry_release(registry);
  g_refused = 0;
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(context.raw(), "");
  EXPECT_EQ(g_refused, 11) << "the callback saw its request refused";
  EXPECT_NE(context.describe().find("Host function failed"), std::string::npos) << context.describe();
}

TEST(ReentryEngine, TheTestNativeThatSumsThreeCallsStillWorksOnThePublicProtocol) {
  Scenario sc = scenario("function g() { return 7; } use resumable; print(resumable(g));\n");
  Result r = go(sc, 0);
  EXPECT_EQ(r.out.raw, "21");
  EXPECT_EQ(r.suspended, 3u);
  Result j = go(sc, 1);
  EXPECT_EQ(j.out.raw, "21");
}

// ---------------------------------------------------------------------------
// The review of story 9: a replay that answers early, a refused re-entry, the template budget, a reader, a
// callback that misbehaves after asking, the record that cannot be made, and the embedded sites not yet covered
// ---------------------------------------------------------------------------

TEST(ReentryReplay, ACastToBoolOfAnInstanceItsCalleeMarkedAsAnErrorLetsTheRecordGo) {
  // `m as bool` asks the type's cast, which asks for f; f marks m as standing for an error, so when the cast runs
  // again the language's own rule answers (false) before the type is asked. The record has nothing to continue: it
  // must go, or its delivery mark takes the return of the next call made from this frame (k's).
  Scenario sc = scenario("use t; m = 0; function f() { global m; use t; t.seterr(m); return 41; } function k() { return 5; }\n"
                         "m = t.ops(f, 0); b = m as bool; q = k(); print(b); print(\"|\"); print(q + 1); q;\n");
  Result r = go(sc, 0);
  EXPECT_EQ(r.out.raw, "false|6");
  EXPECT_EQ(r.out.result, "5");
  EXPECT_EQ(r.pending_depth, 0u);
  EXPECT_EQ(r.suspended, 1u);
  // The same through an operator: the error operand is the value of `+`, and the record goes all the same.
  Scenario add = scenario("use t; m = 0; function f() { global m; use t; t.seterr(m); return 41; } function k() { return 5; }\n"
                          "m = t.ops(f, 0); b = m + 1; q = k(); print(q + 1); q;\n");
  r = go(add, 0);
  EXPECT_EQ(r.out.raw, "6");
  EXPECT_EQ(r.pending_depth, 0u);
}

TEST(ReentryReplay, AResumableNativeRefusedWhenItIsRunAgainLetsItsRecordGo) {
  // g fails the next allocation the process makes, which is the copy of the resumable native's arguments when the
  // CALL runs again to give it g's value: the native's value is Out of memory, and its record goes with it.
  if (!alloc_sweep::available()) {
    GTEST_SKIP() << "no allocation can be failed on this platform";
  }
  Scenario sc = scenario("use t; function g(x) { use t; t.failnext(1); return 7; } function k() { return 5; }\n"
                         "r = t.mapn(1, g); q = k(); print(q + 1); print(\"|\"); r;\n");
  sc.torture = 0;
  sc.moving = 0;
  Result r = go(sc, 0);
  const bool fired = alloc_sweep::fired();
  alloc_sweep::disarm();
  EXPECT_TRUE(fired);
  EXPECT_EQ(r.out.raw, "6|");
  EXPECT_NE(r.out.result.find("Out of memory"), std::string::npos) << r.out.result;
  EXPECT_EQ(r.pending_depth, 0u);
  EXPECT_EQ(r.out.native_depth, 0u);
}

TEST(ReentryReplay, ACallableObjectCalledWithSeventeenArgumentsWhoseCopyIsRefusedIsOutOfMemory) {
  // `self` and sixteen operands are held in the bracket's frame; a seventeenth argument sends the copy to the heap.
  // Refused at a first entry (no record) and at a continuation (whose record goes), the call's value is the failure.
  if (!alloc_sweep::available()) {
    GTEST_SKIP() << "no allocation can be failed on this platform";
  }
  std::string args;
  for (int i = 1; i <= 17; ++i) {
    args += (i > 1 ? ", " : "") + std::to_string(i);
  }
  Scenario first = scenario("use t; function h() { return 3; } v = t.view(h); t.failnext(1); a = v(" + args + "); a;\n");
  first.torture = 0;
  first.moving = 0;
  Result r = go(first, 0);
  bool fired = alloc_sweep::fired();
  alloc_sweep::disarm();
  EXPECT_TRUE(fired);
  EXPECT_NE(r.out.result.find("Out of memory"), std::string::npos) << r.out.result;
  EXPECT_EQ(r.suspended, 0u);
  EXPECT_EQ(r.pending_depth, 0u);
  Scenario again = scenario("use t; function h() { use t; t.failnext(1); return 3; } v = t.view(h); a = v(" + args + "); a;\n");
  again.torture = 0;
  again.moving = 0;
  r = go(again, 0);
  fired = alloc_sweep::fired();
  alloc_sweep::disarm();
  EXPECT_TRUE(fired);
  EXPECT_NE(r.out.result.find("Out of memory"), std::string::npos) << r.out.result;
  EXPECT_EQ(r.suspended, 1u);
  EXPECT_EQ(r.pending_depth, 0u);
  // And with nothing refused, seventeen arguments are an ordinary call.
  r = go(scenario("use t; function h() { return 3; } v = t.view(h); a = v(" + args + "); a;\n"), 0);
  EXPECT_EQ(r.out.result, "3");
}

TEST(ReentryUnwind, ATemplatesOwnBudgetEndingInsideAChainLetsGoOfEveryRecordWhoseInstructionWasInIt) {
  // P's `a + 1` asks for h; h calls the template spin, which has a budget of its own; in spin, `o(1)` is a callable
  // object whose `call` asks for a bound resumable native N, and N asks for g, which loops until spin's budget is
  // spent. The records of `o(1)` and of N were made by an instruction of spin's frame, so they go with it, although
  // N, a native, never had a frame of its own. h gets spin's limit error as its value and returns; P's instruction
  // continues once, from its own record, and N's continuation never runs.
  Compiled spin("use t; function g(x) { k = 0; while (true) { k = k + 1; } return k; } o = t.fwd(t.bind(g)); o(1);", tt::Mode::Script, "spin.tang");
  ASSERT_TRUE(spin.ok());
  Scenario sc = scenario("use t; use spin; function h() { use spin; x = spin(); return 2; } a = t.rx(h, 0, 1); r = a + 1; print(r); r;\n");
  sc.before = [&](Context & c) {
    EXPECT_EQ(gltang_library_add_template(c.library(), "spin", spin.program, 400, GLTANG_SCOPE_EMPTY), GLTANG_OK);
  };
  for (long threshold : {0l, 1l}) {
    g_bound_conts = 0;
    Result r = go(sc, threshold);
    EXPECT_TRUE(r.out.finished) << "threshold " << threshold;
    EXPECT_EQ(r.out.raw, "1007") << "threshold " << threshold;
    const std::vector<std::string> want = {"add left 1 entry step 0", "cont 1 step 0 result 2"};
    EXPECT_EQ(r.log, want) << "threshold " << threshold;
    EXPECT_EQ(r.pending_depth, 0u) << "threshold " << threshold;
    EXPECT_EQ(g_bound_conts.load(), 0) << "N's callee went with spin's frames: its continuation never runs";
    EXPECT_EQ(r.suspended, 3u);
  }
}

TEST(ReentryReader, AnInspectorReadingAValueFromInsideACallbackRunsNoCallbackOfItsType) {
  // A debugger at a poll inside a callback reads a value through the engine's inspector. emb's `to_string` would ask
  // for f, which no instruction could continue; the inspector renders into a buffer sink, which never runs a type's
  // callback (it shows the type's name), so nothing is asked, nested or suspended, from a resumable native (whose body
  // runs at an instruction that suspends) or an opaque one.
  for (const char * native : {"peek", "peekplain"}) {
    Scenario sc = scenario(std::string("use t; function f() { print(\"RAN\"); return 41; } a = t.emb(f); r = t.") + native + "(a); print(\"|\"); r;\n");
    Result r = go(sc, 0);
    EXPECT_EQ(r.out.raw, "|") << native << ": f never ran";
    EXPECT_EQ(r.out.result, "Custom: emb") << native;
    EXPECT_EQ(r.pending_depth, 0u) << native;
    EXPECT_EQ(r.suspended, 0u) << native;
    EXPECT_EQ(r.nested, 0u) << native;
    EXPECT_EQ(r.out.native_depth, 0u) << native;
  }
  // The controls: the inspector reads a value of a built-in kind from inside the native, and the instance printed by
  // the program itself asks, suspends and prints 42.
  Result s = go(scenario("use t; r = t.peek(\"hey\"); r;\n"), 0);
  EXPECT_NE(s.out.result.find("hey"), std::string::npos) << s.out.result;
  Result c = go(scenario("use t; function f() { return 41; } a = t.emb(f); print(a);\n"), 0);
  EXPECT_EQ(c.out.raw, "42");
  EXPECT_EQ(c.suspended, 1u);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  tt::install_watchdog(240);
  return RUN_ALL_TESTS();
}
