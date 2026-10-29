// The remaining operations of a host-defined type (type.h): iteration, calls,
// casts, truthiness, print and to-string, an instance that stands for an error,
// and attributes a host adds to built-in kinds (spec
// spec-tang-host-extensibility story 6; ledger rows D-039 to D-042 and D-045
// to D-047).
//
// The types under test, each driven by a `mode` kept in its raw bytes:
//   coll     iterator_begin and iterator_next: `coll(n, mode)`.
//   callee   call.
//   caster   cast.
//   truthy   truthy.
//   texty    print and to_string.
//   bad      every new operation answers GLTANG_OP_DONE (valid only from an
//            iterator's next), which is `Host function failed` from any other.
//   all      implements every new operation, and logs the operands it was given.
//   decliner implements every new operation and declines each one.
//   plain    implements nothing.
// Each records what it was called with in the log.
//
// Tests that depend on the collector force its arm on inside the test, so that a
// plain `make test` runs them too; the suite is also in TORTURE_BOUNDED.

#include "exec_harness.h"

#include <ghoti.io/cutil/memory.h>
#include <ghoti.io/lang-tang/type.h>

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

struct Bytes {
  int64_t n;
  int64_t mode;
  int64_t pos;
};

struct Types {
  GLTANG_TypeRegistry * registry = nullptr;
  const GLTANG_Type * coll = nullptr;
  const GLTANG_Type * callee = nullptr;
  const GLTANG_Type * caster = nullptr;
  const GLTANG_Type * truthy = nullptr;
  const GLTANG_Type * texty = nullptr;
  const GLTANG_Type * bad = nullptr;
  const GLTANG_Type * all = nullptr;
  const GLTANG_Type * decliner = nullptr;
  const GLTANG_Type * plain = nullptr;
  const GLTANG_Type * eqmark = nullptr;
  const GLTANG_Type * pollcast = nullptr;
  std::vector<std::string> log;
  Context * context = nullptr;
  explicit Types(bool with_attributes = true);
  ~Types() { gltang_type_registry_release(registry); }
  Types(const Types &) = delete;
  Types & operator=(const Types &) = delete;
};

Types * types_of(void * user) {
  return static_cast<Types *>(user);
}

bool read_bytes(GLTANG_NativeCall * call, GLTANG_Handle h, Bytes * b) {
  return gltang_handle_custom_read_bytes(call, h, 0, b, sizeof(*b)) == GLTANG_OK;
}

bool write_bytes(GLTANG_NativeCall * call, GLTANG_Handle h, const Bytes & b) {
  return gltang_handle_custom_write_bytes(call, h, 0, &b, sizeof(b)) == GLTANG_OK;
}

GLTANG_OpStatus answer_text(GLTANG_NativeCall * call, const std::string & text, GLTANG_String_Type tag = GLTANG_UNICODE_STRING_TYPE_TRUSTED) {
  GLTANG_Handle h;
  if (gltang_handle_make_string(call, text.data(), text.size(), tag, &h) != GLTANG_OK) {
    return GLTANG_OP_ERROR;
  }
  gltang_call_return_handle(call, h);
  return GLTANG_OP_OK;
}

// ---------------------------------------------------------------------------
// coll
// ---------------------------------------------------------------------------

// Modes of the iterable: 0 integers 1..n; 1 strings, one made per step; 2 an
// error after the last item; 3 a decline at the second step; 4 begin answers the
// instance itself; 5 begin answers the array [7, 8]; 6 begin answers 3; 7 begin
// declines; 8 begin answers an error; 9 begin answers a plain instance (which has
// no iterator_next).
GLTANG_OpStatus coll_begin(GLTANG_NativeCall * call, void * user, GLTANG_Handle self) {
  Types * t = types_of(user);
  t->log.push_back("coll begin");
  Bytes b;
  if (!read_bytes(call, self, &b)) {
    return GLTANG_OP_ERROR;
  }
  GLTANG_Handle h;
  switch (b.mode) {
    case 4:
      b.pos = 0;
      write_bytes(call, self, b);
      gltang_call_return_handle(call, self);
      return GLTANG_OP_OK;
    case 5: {
      GLTANG_Handle array, x;
      if (gltang_handle_make_array(call, 2, &array) != GLTANG_OK) {
        return GLTANG_OP_ERROR;
      }
      for (int64_t v : {7, 8}) {
        if (gltang_handle_make_integer(call, v, &x) != GLTANG_OK || gltang_handle_array_push(call, array, x) != GLTANG_OK) {
          return GLTANG_OP_ERROR;
        }
      }
      gltang_call_return_handle(call, array);
      return GLTANG_OP_OK;
    }
    case 6:
      gltang_call_return_integer(call, 3);
      return GLTANG_OP_OK;
    case 7:
      return GLTANG_OP_DECLINE;
    case 8:
      gltang_call_return_error(call, GLTANG_ERROR_DIVIDE_BY_ZERO);
      return GLTANG_OP_ERROR;
    case 9:
      if (gltang_handle_make_custom(call, t->plain, &h) != GLTANG_OK) {
        return GLTANG_OP_ERROR;
      }
      gltang_call_return_handle(call, h);
      return GLTANG_OP_OK;
    default:
      break;
  }
  if (gltang_handle_make_custom(call, t->coll, &h) != GLTANG_OK) {
    return GLTANG_OP_ERROR;
  }
  b.pos = 0;
  if (!write_bytes(call, h, b)) {
    return GLTANG_OP_ERROR;
  }
  gltang_call_return_handle(call, h);
  return GLTANG_OP_OK;
}

GLTANG_OpStatus coll_next(GLTANG_NativeCall * call, void * user, GLTANG_Handle self) {
  Types * t = types_of(user);
  Bytes b;
  if (!read_bytes(call, self, &b)) {
    return GLTANG_OP_ERROR;
  }
  t->log.push_back("coll next pos=" + std::to_string(b.pos));
  if (b.mode == 3 && b.pos == 1) {
    return GLTANG_OP_DECLINE;
  }
  if (b.pos >= b.n) {
    if (b.mode == 2) {
      gltang_call_return_error(call, GLTANG_ERROR_MODULO_BY_ZERO);
      return GLTANG_OP_ERROR;
    }
    return GLTANG_OP_DONE;
  }
  ++b.pos;
  if (!write_bytes(call, self, b)) {
    return GLTANG_OP_ERROR;
  }
  if (b.mode == 10 && b.pos == 2) {
    // An item, and then an error set through the call object, with the status OK: the error wins.
    gltang_call_return_integer(call, 99);
    gltang_call_return_error(call, GLTANG_ERROR_NOT_A_NUMBER);
    return GLTANG_OP_OK;
  }
  if (b.mode == 1) {
    return answer_text(call, "item" + std::to_string(b.pos));
  }
  gltang_call_return_integer(call, b.pos);
  return GLTANG_OP_OK;
}

// ---------------------------------------------------------------------------
// callee, caster, truthy, texty
// ---------------------------------------------------------------------------

// Modes: 0 answers argc * 1000 + the first argument when it is an integer; 1
// declines; 2 answers an error; 3 answers nothing (null).
GLTANG_OpStatus callee_call(GLTANG_NativeCall * call, void * user, GLTANG_Handle self, const GLTANG_Handle * args, size_t argc) {
  Types * t = types_of(user);
  Bytes b;
  read_bytes(call, self, &b);
  t->log.push_back("callee call argc=" + std::to_string(argc));
  switch (b.mode) {
    case 1: return GLTANG_OP_DECLINE;
    case 2:
      gltang_call_return_error(call, GLTANG_ERROR_NOT_A_NUMBER);
      return GLTANG_OP_ERROR;
    case 3: return GLTANG_OP_OK;
    default: break;
  }
  int64_t first = argc && gltang_handle_kind(call, args[0]) == GLTANG_KIND_INTEGER ? gltang_handle_integer(call, args[0]) : 0;
  gltang_call_return_integer(call, static_cast<int64_t>(argc) * 1000 + first);
  return GLTANG_OP_OK;
}

// Modes: 0 answers every target; 1 only int and float; 2 declines all; 3 errors; 4 answers the wrong kind.
GLTANG_OpStatus caster_cast(GLTANG_NativeCall * call, void * user, GLTANG_Handle self, GLTANG_CastTarget target) {
  Types * t = types_of(user);
  Bytes b;
  read_bytes(call, self, &b);
  t->log.push_back("caster cast " + std::to_string(static_cast<int>(target)));
  if (b.mode == 2) {
    return GLTANG_OP_DECLINE;
  }
  if (b.mode == 3) {
    gltang_call_return_error(call, GLTANG_ERROR_NOT_A_NUMBER);
    return GLTANG_OP_ERROR;
  }
  if (b.mode == 4) {
    // The wrong kind for every target: a string for a number, an integer for a bool or a string.
    if (target == GLTANG_CAST_TARGET_INT || target == GLTANG_CAST_TARGET_FLOAT) {
      return answer_text(call, "wrong");
    }
    gltang_call_return_integer(call, 7);
    return GLTANG_OP_OK;
  }
  switch (target) {
    case GLTANG_CAST_TARGET_INT: gltang_call_return_integer(call, 41); return GLTANG_OP_OK;
    case GLTANG_CAST_TARGET_FLOAT: gltang_call_return_float(call, 2.5); return GLTANG_OP_OK;
    case GLTANG_CAST_TARGET_BOOL:
      if (b.mode == 1) {
        return GLTANG_OP_DECLINE;
      }
      gltang_call_return_bool(call, false);
      return GLTANG_OP_OK;
    case GLTANG_CAST_TARGET_STRING:
      if (b.mode == 1) {
        return GLTANG_OP_DECLINE;
      }
      return answer_text(call, "castS");
  }
  return GLTANG_OP_DECLINE;
}

// Modes: 0 false; 1 true; 2 declines; 3 errors; 4 sets an error through the call object and answers OK.
GLTANG_OpStatus truthy_truthy(GLTANG_NativeCall * call, void * user, GLTANG_Handle self, bool * out) {
  Types * t = types_of(user);
  Bytes b;
  read_bytes(call, self, &b);
  t->log.push_back("truthy truthy");
  if (b.mode == 2) {
    return GLTANG_OP_DECLINE;
  }
  if (b.mode == 3) {
    // Not a marker kind: a marker error prints itself and is never recorded as discarded.
    gltang_call_return_error(call, GLTANG_ERROR_DIVIDE_BY_ZERO);
    return GLTANG_OP_ERROR;
  }
  if (b.mode == 4) {
    // An error set through the call object, with the status OK.
    *out = true;
    gltang_call_return_error(call, GLTANG_ERROR_NOT_A_NUMBER);
    return GLTANG_OP_OK;
  }
  *out = b.mode == 1;
  return GLTANG_OP_OK;
}

// Modes: 0 both (print gives HTML-tagged "<b>&</b>", to_string gives "T<&>"); 1 print only (to_string
// declines); 2 to_string only (print declines); 3 print answers an integer; 4 to_string answers an integer; 5 print errors; 6 print answers
// two segments (text "a<", HTML "<i>"); 7 print declines (to_string answers); 8 to_string errors.
GLTANG_OpStatus texty_print(GLTANG_NativeCall * call, void * user, GLTANG_Handle self) {
  Types * t = types_of(user);
  Bytes b;
  read_bytes(call, self, &b);
  t->log.push_back("texty print");
  switch (b.mode) {
    case 3:
      gltang_call_return_integer(call, 5);
      return GLTANG_OP_OK;
    case 5:
      gltang_call_return_error(call, GLTANG_ERROR_NOT_A_NUMBER);
      return GLTANG_OP_ERROR;
    case 6: {
      GLTANG_Handle a, h, both;
      if (gltang_handle_make_string(call, "a<", 2, GLTANG_UNICODE_STRING_TYPE_TRUSTED, &a) != GLTANG_OK ||
          gltang_handle_make_string(call, "<i>", 3, GLTANG_UNICODE_STRING_TYPE_HTML, &h) != GLTANG_OK ||
          gltang_handle_make_concat(call, a, h, &both) != GLTANG_OK) {
        return GLTANG_OP_ERROR;
      }
      gltang_call_return_handle(call, both);
      return GLTANG_OP_OK;
    }
    case 2:
    case 7: return GLTANG_OP_DECLINE;
    default: return answer_text(call, "<b>&</b>", GLTANG_UNICODE_STRING_TYPE_HTML);
  }
}

GLTANG_OpStatus texty_to_string(GLTANG_NativeCall * call, void * user, GLTANG_Handle self) {
  Types * t = types_of(user);
  Bytes b;
  read_bytes(call, self, &b);
  t->log.push_back("texty to_string");
  if (b.mode == 1) {
    return GLTANG_OP_DECLINE;
  }
  if (b.mode == 4) {
    gltang_call_return_integer(call, 5);
    return GLTANG_OP_OK;
  }
  if (b.mode == 8) {
    gltang_call_return_error(call, GLTANG_ERROR_NOT_A_NUMBER);
    return GLTANG_OP_ERROR;
  }
  return answer_text(call, "T<&>");
}

// ---------------------------------------------------------------------------
// bad, all, decliner
// ---------------------------------------------------------------------------

GLTANG_OpStatus bad_begin(GLTANG_NativeCall *, void *, GLTANG_Handle) { return GLTANG_OP_DONE; }
GLTANG_OpStatus bad_call(GLTANG_NativeCall *, void *, GLTANG_Handle, const GLTANG_Handle *, size_t) { return GLTANG_OP_DONE; }
GLTANG_OpStatus bad_cast(GLTANG_NativeCall *, void *, GLTANG_Handle, GLTANG_CastTarget) { return GLTANG_OP_DONE; }
GLTANG_OpStatus bad_truthy(GLTANG_NativeCall *, void *, GLTANG_Handle, bool *) { return GLTANG_OP_DONE; }
GLTANG_OpStatus bad_text(GLTANG_NativeCall *, void *, GLTANG_Handle) { return GLTANG_OP_DONE; }
GLTANG_OpStatus bad_add(GLTANG_NativeCall *, void *, GLTANG_Handle, GLTANG_Handle, bool, bool) { return GLTANG_OP_DONE; }

GLTANG_OpStatus all_begin(GLTANG_NativeCall * call, void * user, GLTANG_Handle self) {
  types_of(user)->log.push_back("all begin");
  // The array [1, 2], so the loop runs; the operand is the instance.
  GLTANG_Handle array, x;
  if (gltang_handle_custom_type(call, self) == nullptr || gltang_handle_make_array(call, 2, &array) != GLTANG_OK) {
    return GLTANG_OP_ERROR;
  }
  for (int64_t v : {1, 2}) {
    if (gltang_handle_make_integer(call, v, &x) != GLTANG_OK || gltang_handle_array_push(call, array, x) != GLTANG_OK) {
      return GLTANG_OP_ERROR;
    }
  }
  gltang_call_return_handle(call, array);
  return GLTANG_OP_OK;
}
GLTANG_OpStatus all_next(GLTANG_NativeCall *, void * user, GLTANG_Handle) {
  types_of(user)->log.push_back("all next");
  return GLTANG_OP_DONE;
}
GLTANG_OpStatus all_call(GLTANG_NativeCall * call, void * user, GLTANG_Handle self, const GLTANG_Handle * args, size_t argc) {
  std::string kinds;
  for (size_t i = 0; i < argc; ++i) {
    kinds += std::to_string(static_cast<int>(gltang_handle_kind(call, args[i]))) + ",";
  }
  types_of(user)->log.push_back("all call argc=" + std::to_string(argc) + " kinds=" + kinds +
                                " self_custom=" + std::to_string(gltang_handle_custom_type(call, self) != nullptr));
  gltang_call_return_integer(call, 77);
  return GLTANG_OP_OK;
}
GLTANG_OpStatus all_cast(GLTANG_NativeCall * call, void * user, GLTANG_Handle, GLTANG_CastTarget target) {
  types_of(user)->log.push_back("all cast " + std::to_string(static_cast<int>(target)));
  switch (target) {
    case GLTANG_CAST_TARGET_INT: gltang_call_return_integer(call, 5); break;
    case GLTANG_CAST_TARGET_FLOAT: gltang_call_return_float(call, 2.5); break;
    case GLTANG_CAST_TARGET_BOOL: gltang_call_return_bool(call, true); break;
    case GLTANG_CAST_TARGET_STRING: return answer_text(call, "5");
  }
  return GLTANG_OP_OK;
}
GLTANG_OpStatus all_truthy(GLTANG_NativeCall *, void * user, GLTANG_Handle, bool * out) {
  types_of(user)->log.push_back("all truthy");
  *out = false;
  return GLTANG_OP_OK;
}
GLTANG_OpStatus all_print(GLTANG_NativeCall * call, void * user, GLTANG_Handle) {
  types_of(user)->log.push_back("all print");
  return answer_text(call, "P");
}
GLTANG_OpStatus all_to_string(GLTANG_NativeCall * call, void * user, GLTANG_Handle) {
  types_of(user)->log.push_back("all to_string");
  return answer_text(call, "S");
}

GLTANG_OpStatus dec_begin(GLTANG_NativeCall *, void * user, GLTANG_Handle) {
  types_of(user)->log.push_back("dec begin");
  return GLTANG_OP_DECLINE;
}
GLTANG_OpStatus dec_next(GLTANG_NativeCall *, void * user, GLTANG_Handle) {
  types_of(user)->log.push_back("dec next");
  return GLTANG_OP_DECLINE;
}
GLTANG_OpStatus dec_call(GLTANG_NativeCall *, void * user, GLTANG_Handle, const GLTANG_Handle *, size_t) {
  types_of(user)->log.push_back("dec call");
  return GLTANG_OP_DECLINE;
}
GLTANG_OpStatus dec_cast(GLTANG_NativeCall *, void * user, GLTANG_Handle, GLTANG_CastTarget) {
  types_of(user)->log.push_back("dec cast");
  return GLTANG_OP_DECLINE;
}
GLTANG_OpStatus dec_truthy(GLTANG_NativeCall *, void * user, GLTANG_Handle, bool *) {
  types_of(user)->log.push_back("dec truthy");
  return GLTANG_OP_DECLINE;
}
GLTANG_OpStatus dec_text(GLTANG_NativeCall *, void * user, GLTANG_Handle) {
  types_of(user)->log.push_back("dec text");
  return GLTANG_OP_DECLINE;
}

// eqmark: `equal` answers a new instance of plain marked as standing for an error.
GLTANG_OpStatus eqmark_equal(GLTANG_NativeCall * call, void * user, GLTANG_Handle, GLTANG_Handle, bool) {
  Types * t = types_of(user);
  GLTANG_Handle h;
  if (gltang_handle_make_custom(call, t->plain, &h) != GLTANG_OK || gltang_handle_custom_set_error(call, h, true) != GLTANG_OK) {
    return GLTANG_OP_ERROR;
  }
  gltang_call_return_handle(call, h);
  return GLTANG_OP_OK;
}

// pollcast: mode 0 answers the right kind for each target; mode 1 polls for good first, so a
// budget ends the run inside the cast.
GLTANG_OpStatus pollcast_cast(GLTANG_NativeCall * call, void * user, GLTANG_Handle self, GLTANG_CastTarget target) {
  Types * t = types_of(user);
  Bytes b;
  read_bytes(call, self, &b);
  if (b.mode == 1) {
    for (int i = 0; i < 100000; ++i) {
      if (gltang_call_poll(call, 100) != GLTANG_OK) {
        return GLTANG_OP_DECLINE;  // the run is to stop: whatever is answered, it unwinds
      }
      if (i == 0) {
        // The identity the first poll wrote in the top frame: where the run stands, as a debugger would read it.
        GRCORE_Stack * stack = grcore_context_stack(t->context->context);
        GRCORE_PollIdentity id = {0, 0};
        (void)grcore_stack_identity(stack, grcore_stack_top(stack), &id);
        t->log.push_back("identity " + std::to_string(id.function) + ":" + std::to_string(id.offset));
      }
    }
  }
  switch (target) {
    case GLTANG_CAST_TARGET_INT: gltang_call_return_integer(call, 3); break;
    case GLTANG_CAST_TARGET_FLOAT: gltang_call_return_float(call, 1.5); break;
    case GLTANG_CAST_TARGET_BOOL: gltang_call_return_bool(call, true); break;
    case GLTANG_CAST_TARGET_STRING: return answer_text(call, "p");
  }
  return GLTANG_OP_OK;
}

// ---------------------------------------------------------------------------
// Attributes of built-in kinds
// ---------------------------------------------------------------------------

GLTANG_OpStatus attr_slug(GLTANG_NativeCall * call, void * user, GLTANG_Handle self) {
  types_of(user)->log.push_back("attr slug");
  size_t length = 0;
  const char * text = gltang_handle_text(call, self, &length);
  return answer_text(call, "slug:" + std::string(text ? text : "", length));
}
GLTANG_OpStatus attr_wrong(GLTANG_NativeCall * call, void * user, GLTANG_Handle) {
  types_of(user)->log.push_back("attr wrong");
  return answer_text(call, "WRONG");
}
GLTANG_OpStatus attr_double(GLTANG_NativeCall * call, void * user, GLTANG_Handle self) {
  types_of(user)->log.push_back("attr double");
  gltang_call_return_integer(call, gltang_handle_integer(call, self) * 2);
  return GLTANG_OP_OK;
}
GLTANG_OpStatus attr_kind(GLTANG_NativeCall * call, void * user, GLTANG_Handle self) {
  types_of(user)->log.push_back("attr kind");
  gltang_call_return_integer(call, static_cast<int64_t>(gltang_handle_kind(call, self)));
  return GLTANG_OP_OK;
}
GLTANG_OpStatus attr_decline(GLTANG_NativeCall *, void * user, GLTANG_Handle) {
  types_of(user)->log.push_back("attr decline");
  return GLTANG_OP_DECLINE;
}
GLTANG_OpStatus attr_error(GLTANG_NativeCall * call, void *, GLTANG_Handle) {
  gltang_call_return_error(call, GLTANG_ERROR_NOT_A_NUMBER);
  return GLTANG_OP_ERROR;
}
GLTANG_OpStatus attr_array(GLTANG_NativeCall * call, void *, GLTANG_Handle self) {
  // A fresh string per read, so the collector has work to do under torture.
  gltang_call_return_integer(call, static_cast<int64_t>(gltang_handle_array_length(call, self)) + 100);
  return GLTANG_OP_OK;
}

Types::Types(bool with_attributes) {
  EXPECT_EQ(gltang_type_registry_create(&registry), GLTANG_OK);
  auto base = [&](const char * name) {
    GLTANG_TypeDescriptor d = GLTANG_TYPE_DESCRIPTOR_INIT;
    d.name = name;
    d.byte_count = sizeof(Bytes);
    d.slot_count = 1;
    d.thread_safe = true;
    d.user = this;
    return d;
  };
  GLTANG_TypeDescriptor d = base("coll");
  d.iterator_begin = coll_begin;
  d.iterator_next = coll_next;
  EXPECT_EQ(gltang_type_registry_add(registry, &d, &coll), GLTANG_OK);
  d = base("callee");
  d.call = callee_call;
  EXPECT_EQ(gltang_type_registry_add(registry, &d, &callee), GLTANG_OK);
  d = base("caster");
  d.cast = caster_cast;
  EXPECT_EQ(gltang_type_registry_add(registry, &d, &caster), GLTANG_OK);
  d = base("truthy");
  d.truthy = truthy_truthy;
  EXPECT_EQ(gltang_type_registry_add(registry, &d, &truthy), GLTANG_OK);
  d = base("texty");
  d.print = texty_print;
  d.to_string = texty_to_string;
  EXPECT_EQ(gltang_type_registry_add(registry, &d, &texty), GLTANG_OK);
  d = base("bad");
  d.add = bad_add;
  d.iterator_begin = bad_begin;
  d.iterator_next = bad_begin;
  d.call = bad_call;
  d.cast = bad_cast;
  d.truthy = bad_truthy;
  d.print = bad_text;
  d.to_string = bad_text;
  EXPECT_EQ(gltang_type_registry_add(registry, &d, &bad), GLTANG_OK);
  d = base("all");
  d.iterator_begin = all_begin;
  d.iterator_next = all_next;
  d.call = all_call;
  d.cast = all_cast;
  d.truthy = all_truthy;
  d.print = all_print;
  d.to_string = all_to_string;
  EXPECT_EQ(gltang_type_registry_add(registry, &d, &all), GLTANG_OK);
  d = base("decliner");
  d.iterator_begin = dec_begin;
  d.iterator_next = dec_next;
  d.call = dec_call;
  d.cast = dec_cast;
  d.truthy = dec_truthy;
  d.print = dec_text;
  d.to_string = dec_text;
  EXPECT_EQ(gltang_type_registry_add(registry, &d, &decliner), GLTANG_OK);
  d = base("plain");
  EXPECT_EQ(gltang_type_registry_add(registry, &d, &plain), GLTANG_OK);
  d = base("eqmark");
  d.equal = eqmark_equal;
  EXPECT_EQ(gltang_type_registry_add(registry, &d, &eqmark), GLTANG_OK);
  d = base("pollcast");
  d.cast = pollcast_cast;
  EXPECT_EQ(gltang_type_registry_add(registry, &d, &pollcast), GLTANG_OK);
  if (with_attributes) {
    EXPECT_EQ(gltang_type_registry_add_attribute(registry, GLTANG_KIND_STRING, "slug", attr_slug, this), GLTANG_OK);
    EXPECT_EQ(gltang_type_registry_add_attribute(registry, GLTANG_KIND_STRING, "html", attr_wrong, this), GLTANG_OK);
    EXPECT_EQ(gltang_type_registry_add_attribute(registry, GLTANG_KIND_STRING, "length", attr_wrong, this), GLTANG_OK);
    EXPECT_EQ(gltang_type_registry_add_attribute(registry, GLTANG_KIND_STRING, "nope", attr_decline, this), GLTANG_OK);
    EXPECT_EQ(gltang_type_registry_add_attribute(registry, GLTANG_KIND_STRING, "boom", attr_error, this), GLTANG_OK);
    EXPECT_EQ(gltang_type_registry_add_attribute(registry, GLTANG_KIND_ARRAY, "size", attr_wrong, this), GLTANG_OK);
    EXPECT_EQ(gltang_type_registry_add_attribute(registry, GLTANG_KIND_ARRAY, "extra", attr_array, this), GLTANG_OK);
    EXPECT_EQ(gltang_type_registry_add_attribute(registry, GLTANG_KIND_INTEGER, "double", attr_double, this), GLTANG_OK);
    EXPECT_EQ(gltang_type_registry_add_attribute(registry, GLTANG_KIND_FLOAT, "kind", attr_kind, this), GLTANG_OK);
    EXPECT_EQ(gltang_type_registry_add_attribute(registry, GLTANG_KIND_BOOL, "kind", attr_kind, this), GLTANG_OK);
    EXPECT_EQ(gltang_type_registry_add_attribute(registry, GLTANG_KIND_FUNCTION, "kind", attr_kind, this), GLTANG_OK);
    EXPECT_EQ(gltang_type_registry_add_attribute(registry, GLTANG_KIND_RNG, "kind", attr_kind, this), GLTANG_OK);
    EXPECT_EQ(gltang_type_registry_add_attribute(registry, GLTANG_KIND_RNG, "next_int", attr_wrong, this), GLTANG_OK);
  }
}

// ---------------------------------------------------------------------------
// The natives a program makes instances with
// ---------------------------------------------------------------------------

template <const GLTANG_Type * Types::*Member>
bool make_with_mode(GLTANG_NativeCall * call, void * user) {
  Types * t = types_of(user);
  Bytes b = {0, 0, 0};
  if (gltang_call_count(call) > 0) {
    b.mode = gltang_call_integer(call, 0);
  }
  if (gltang_call_count(call) > 1) {
    b.n = gltang_call_integer(call, 1);
  }
  GLTANG_Handle h;
  if (gltang_handle_make_custom(call, t->*Member, &h) != GLTANG_OK || !write_bytes(call, h, b)) {
    return false;
  }
  gltang_call_return_handle(call, h);
  return true;
}

// coll(n, mode) takes its arguments the other way round, which reads better in a program.
bool make_coll(GLTANG_NativeCall * call, void * user) {
  Types * t = types_of(user);
  Bytes b = {gltang_call_integer(call, 0), gltang_call_count(call) > 1 ? gltang_call_integer(call, 1) : 0, 0};
  GLTANG_Handle h;
  if (gltang_handle_make_custom(call, t->coll, &h) != GLTANG_OK || !write_bytes(call, h, b)) {
    return false;
  }
  gltang_call_return_handle(call, h);
  return true;
}

/// mark(x): marks x as standing for an error, and answers x.
bool mark(GLTANG_NativeCall * call, void *) {
  GLTANG_Handle x;
  if (gltang_call_arg(call, 0, &x) != GLTANG_OK) {
    return false;
  }
  gltang_call_return_integer(call, gltang_handle_custom_set_error(call, x, gltang_call_count(call) < 2 || gltang_call_bool(call, 1)) == GLTANG_OK ? 1 : 0);
  return true;
}

/// marked(x): whether x is marked, by the reader.
bool marked(GLTANG_NativeCall * call, void *) {
  GLTANG_Handle x;
  if (gltang_call_arg(call, 0, &x) != GLTANG_OK) {
    return false;
  }
  gltang_call_return_bool(call, gltang_handle_custom_is_error(call, x));
  return true;
}

bool allocs(GLTANG_NativeCall * call, void * user) {
  GRHEAP_Stats stats;
  EXPECT_EQ(grheap_stats(types_of(user)->context->heap, &stats), GRHEAP_OK);
  gltang_call_return_integer(call, static_cast<int64_t>(stats.allocations));
  return true;
}

bool mallocs(GLTANG_NativeCall * call, void * user) {
  gltang_call_return_integer(call, types_of(user)->context->tracker.calls);
  return true;
}

/// same(x, y): whether two handles name the same object.
bool collect(GLTANG_NativeCall *, void * user) {
  EXPECT_EQ(grheap_collect(types_of(user)->context->heap), GRHEAP_OK);
  return true;
}

struct Out {
  bool finished = false;
  std::string described;
  std::string raw;
  std::string rendered;
  GLTANG_ValueKind kind = GLTANG_KIND_NULL;
  GLTANG_ErrorKind error = GLTANG_ERROR_KIND_COUNT;
  int64_t integer = 0;
  bool boolean = false;
  size_t errors_listed = 0;
  uint64_t compiled_functions = 0;
  uint64_t deopts = 0;
  uint64_t fuel = 0;
  uint64_t position = 0;
  uint64_t native_depth = 0;
  bool fired = false;
  std::vector<GLTANG_ErrorHow> hows;
  std::vector<GLTANG_ErrorKind> error_kinds;
  std::vector<std::string> log;
};

using Setup = std::function<void(Context &, Types &)>;

/// While an allocation failure is being injected, the setup calls of `run` may fail without that being a defect of the test.
static bool g_injecting = false;
#define CHECK_EQ(a, b) do { if (!g_injecting) { EXPECT_EQ(a, b); } else { (void)((a) == (b)); } } while (0)
#define CHECK_TRUE(x) do { if (!g_injecting) { EXPECT_TRUE(x); } else { (void)(x); } } while (0)

Out run(const std::string & source, Types & types, const Config & config = Config(), const Setup & setup = Setup(), bool attach = true) {
  Out out;
  Compiled compiled(source);
  EXPECT_TRUE(compiled.ok()) << source << ": " << compiled.error.message;
  if (!compiled.ok()) {
    return out;
  }
  Context context(compiled.program, config);
  CHECK_TRUE(context.ok());
  if (!context.ok()) {
    return out;
  }
  types.context = &context;
  types.log.clear();
  GLTANG_Library * t = nullptr;
  CHECK_EQ(gltang_library_create("t", &t), GLTANG_OK);
  CHECK_EQ(gltang_library_add_native(t, "coll", make_coll, &types), GLTANG_OK);
  CHECK_EQ(gltang_library_add_native(t, "callee", make_with_mode<&Types::callee>, &types), GLTANG_OK);
  CHECK_EQ(gltang_library_add_native(t, "caster", make_with_mode<&Types::caster>, &types), GLTANG_OK);
  CHECK_EQ(gltang_library_add_native(t, "truthy", make_with_mode<&Types::truthy>, &types), GLTANG_OK);
  CHECK_EQ(gltang_library_add_native(t, "texty", make_with_mode<&Types::texty>, &types), GLTANG_OK);
  CHECK_EQ(gltang_library_add_native(t, "bad", make_with_mode<&Types::bad>, &types), GLTANG_OK);
  CHECK_EQ(gltang_library_add_native(t, "all", make_with_mode<&Types::all>, &types), GLTANG_OK);
  CHECK_EQ(gltang_library_add_native(t, "decliner", make_with_mode<&Types::decliner>, &types), GLTANG_OK);
  CHECK_EQ(gltang_library_add_native(t, "plain", make_with_mode<&Types::plain>, &types), GLTANG_OK);
  CHECK_EQ(gltang_library_add_native(t, "eqmark", make_with_mode<&Types::eqmark>, &types), GLTANG_OK);
  CHECK_EQ(gltang_library_add_native(t, "pollcast", make_with_mode<&Types::pollcast>, &types), GLTANG_OK);
  CHECK_EQ(gltang_library_add_native(t, "mark", mark, &types), GLTANG_OK);
  CHECK_EQ(gltang_library_add_native(t, "marked", marked, &types), GLTANG_OK);
  CHECK_EQ(gltang_library_add_native(t, "allocs", allocs, &types), GLTANG_OK);
  CHECK_EQ(gltang_library_add_native(t, "mallocs", mallocs, &types), GLTANG_OK);
  CHECK_EQ(gltang_library_add_native(t, "collect", collect, &types), GLTANG_OK);
  CHECK_EQ(gltang_library_add_library(context.library(), t), GLTANG_OK);
  gltang_library_release(t);
  if (attach) {
    CHECK_EQ(gltang_execution_attach_types(context.execution, types.registry), GLTANG_OK);
  }
  if (setup) {
    setup(context, types);
  }
  out.finished = context.execute();
  out.raw = context.raw();
  out.rendered = context.rendered();
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
  GLTANG_JitStats jit = context.jit_stats();
  out.compiled_functions = jit.functions_compiled;
  out.deopts = jit.deopts;
  out.fuel = grcore_context_fuel_used(context.context);
  out.position = gltang_vm_test_position(context.execution);
  out.native_depth = grcore_context_depth(context.context, GRCORE_DEPTH_NATIVE);
  out.fired = context.tracker.fired;
  for (size_t i = 0; i < out.errors_listed; ++i) {
    out.hows.push_back(context.error(i).e.how);
    out.error_kinds.push_back(context.error(i).e.kind);
  }
  out.log = types.log;
  types.context = nullptr;
  return out;
}

using Log = std::vector<std::string>;

// A build without the JIT runs every program in the interpreter; the differential then compares it with itself.
#ifdef GLTANG_WITH_JIT
#define EXPECT_COMPILED(o, source) EXPECT_GT((o).compiled_functions, 0u) << (source)
#else
#define EXPECT_COMPILED(o, source) EXPECT_TRUE(true) << (source)
#endif

Config forced_torture() {
  Config c;
  c.torture = 1;
  c.verify = 1;
  return c;
}

Config no_instruments() {
  Config c;
  c.jit_threshold = 0;
  c.torture = 0;
  c.verify = 0;
  c.moving_stack = 0;
  c.relocate = 0;
  return c;
}

#define PRELUDE                                                                                                     \
  "use t; coll = t.coll; callee = t.callee; caster = t.caster; truthy = t.truthy; texty = t.texty; bad = t.bad; " \
  "all = t.all; decliner = t.decliner; plain = t.plain; eqmark = t.eqmark; pollcast = t.pollcast; mark = t.mark; marked = t.marked; "

}  // namespace

// ---------------------------------------------------------------------------
// The descriptor
// ---------------------------------------------------------------------------

TEST(TypeOps, TheNewMembersAreAtTheEndInTheDocumentedOrderAndTheMinimumSizeDidNotMove) {
  using D = GLTANG_TypeDescriptor;
  EXPECT_EQ(GLTANG_TYPE_DESCRIPTOR_MIN_SIZE, offsetof(D, slice) + sizeof(GLTANG_TypeSliceFn));
  EXPECT_LT(offsetof(D, slice), offsetof(D, iterator_begin));
  EXPECT_LT(offsetof(D, iterator_begin), offsetof(D, iterator_next));
  EXPECT_LT(offsetof(D, iterator_next), offsetof(D, call));
  EXPECT_LT(offsetof(D, call), offsetof(D, cast));
  EXPECT_LT(offsetof(D, cast), offsetof(D, truthy));
  EXPECT_LT(offsetof(D, truthy), offsetof(D, print));
  EXPECT_LT(offsetof(D, print), offsetof(D, to_string));
  EXPECT_LT(offsetof(D, to_string), offsetof(D, resumable_ops));
  EXPECT_LT(offsetof(D, resumable_ops), offsetof(D, continuations));
  EXPECT_LT(offsetof(D, continuations), offsetof(D, continuation_count));
  EXPECT_EQ(offsetof(D, continuation_count) + sizeof(size_t), sizeof(D));
  D d = GLTANG_TYPE_DESCRIPTOR_INIT;
  EXPECT_EQ(d.size, sizeof(D));
  EXPECT_EQ(d.iterator_begin, nullptr);
  EXPECT_EQ(d.to_string, nullptr);
  EXPECT_EQ(GLTANG_OP_DONE, 3) << "appended to the status enum: the three before it keep their numbers";
  EXPECT_EQ(GLTANG_OP_PENDING, 4);
  EXPECT_EQ(d.resumable_ops, 0u);
  EXPECT_EQ(d.continuations, nullptr);
  EXPECT_EQ(d.continuation_count, 0u);
}

TEST(TypeOps, ADescriptorSizedForTheFirstVersionWorksAndHasNoNewOperation) {
  GLTANG_TypeRegistry * r = nullptr;
  ASSERT_EQ(gltang_type_registry_create(&r), GLTANG_OK);
  // Garbage beyond the old size, which the engine must not read.
  struct {
    GLTANG_TypeDescriptor d;
    char tail[64];
  } big;
  std::memset(&big, 0xAB, sizeof big);
  big.d = GLTANG_TYPE_DESCRIPTOR_INIT;
  big.d.size = GLTANG_TYPE_DESCRIPTOR_MIN_SIZE;
  big.d.name = "old";
  // The engine's copy has these members as the old size leaves them: empty.
  big.d.iterator_begin = reinterpret_cast<GLTANG_TypeIteratorBeginFn>(0x1);
  big.d.to_string = reinterpret_cast<GLTANG_TypeToStringFn>(0x1);
  const GLTANG_Type * type = nullptr;
  ASSERT_EQ(gltang_type_registry_add(r, &big.d, &type), GLTANG_OK);
  // The same declaration without the new members hashes the same.
  GLTANG_TypeDescriptor same = GLTANG_TYPE_DESCRIPTOR_INIT;
  same.name = "old";
  GLTANG_TypeRegistry * r2 = nullptr;
  ASSERT_EQ(gltang_type_registry_create(&r2), GLTANG_OK);
  const GLTANG_Type * type2 = nullptr;
  ASSERT_EQ(gltang_type_registry_add(r2, &same, &type2), GLTANG_OK);
  EXPECT_EQ(gltang_type_table_hash(type), gltang_type_table_hash(type2)) << "a size that does not cover the new members leaves them out";
  gltang_type_registry_release(r);
  gltang_type_registry_release(r2);
}

TEST(TypeOps, ATypeThatImplementsNoneOfTheNewOperationsHashesAsItDidBeforeStory6) {
  // The hash of a type declaring a `money`'s old operations, as computed by story 5's code (the
  // operation bits 0 to 16): the FNV-1a of the declaration. The expected value is recomputed here
  // from the documented inputs so that a new operation's bit moves it only when the type uses it.
  auto fnv = [](uint64_t h, const void * bytes, size_t length) {
    const unsigned char * p = static_cast<const unsigned char *>(bytes);
    for (size_t i = 0; i < length; ++i) {
      h ^= p[i];
      h *= UINT64_C(0x100000001b3);
    }
    return h;
  };
  auto fnv_u64 = [&](uint64_t h, uint64_t v) {
    unsigned char b[8];
    for (size_t i = 0; i < 8; ++i) {
      b[i] = static_cast<unsigned char>(v >> (8u * i));
    }
    return fnv(h, b, sizeof b);
  };
  auto expected = [&](const char * name, uint32_t version, uint32_t slots, uint32_t bytes, bool safe, uint32_t ops) {
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    h = fnv(h, name, std::strlen(name));
    h = fnv_u64(h, std::strlen(name));
    h = fnv_u64(h, version);
    h = fnv_u64(h, slots);
    h = fnv_u64(h, bytes);
    h = fnv_u64(h, safe ? 1u : 0u);
    return fnv_u64(h, ops);
  };
  GLTANG_TypeRegistry * r = nullptr;
  ASSERT_EQ(gltang_type_registry_create(&r), GLTANG_OK);
  GLTANG_TypeDescriptor d = GLTANG_TYPE_DESCRIPTOR_INIT;
  d.name = "m";
  d.version = 2;
  d.slot_count = 1;
  d.byte_count = 8;
  d.thread_safe = true;
  d.add = [](GLTANG_NativeCall *, void *, GLTANG_Handle, GLTANG_Handle, bool, bool) { return GLTANG_OP_DECLINE; };
  d.slice = [](GLTANG_NativeCall *, void *, GLTANG_Handle, GLTANG_Handle, GLTANG_Handle, GLTANG_Handle) { return GLTANG_OP_DECLINE; };
  const GLTANG_Type * t = nullptr;
  ASSERT_EQ(gltang_type_registry_add(r, &d, &t), GLTANG_OK);
  // add is bit 0, slice is bit 16.
  EXPECT_EQ(gltang_type_table_hash(t), expected("m", 2, 1, 8, true, (1u << 0) | (1u << 16)));
  // Each new operation adds exactly its own bit, after the old ones.
  struct One {
    const char * name;
    std::function<void(GLTANG_TypeDescriptor &)> set;
    unsigned bit;
  } ones[] = {
    {"a", [](GLTANG_TypeDescriptor & x) { x.iterator_begin = dec_begin; }, 17},
    {"b", [](GLTANG_TypeDescriptor & x) { x.iterator_next = dec_next; }, 18},
    {"c", [](GLTANG_TypeDescriptor & x) { x.call = dec_call; }, 19},
    {"d", [](GLTANG_TypeDescriptor & x) { x.cast = dec_cast; }, 20},
    {"e", [](GLTANG_TypeDescriptor & x) { x.truthy = dec_truthy; }, 21},
    {"f", [](GLTANG_TypeDescriptor & x) { x.print = dec_text; }, 22},
    {"g", [](GLTANG_TypeDescriptor & x) { x.to_string = dec_text; }, 23},
  };
  for (const One & one : ones) {
    GLTANG_TypeDescriptor x = GLTANG_TYPE_DESCRIPTOR_INIT;
    x.name = one.name;
    one.set(x);
    const GLTANG_Type * tt = nullptr;
    ASSERT_EQ(gltang_type_registry_add(r, &x, &tt), GLTANG_OK);
    EXPECT_EQ(gltang_type_table_hash(tt), expected(one.name, 0, 0, 0, false, 1u << one.bit)) << one.name;
  }
  gltang_type_registry_release(r);
}

// ---------------------------------------------------------------------------
// A type that implements nothing, and one that implements everything
// ---------------------------------------------------------------------------

TEST(TypeOps, ATypeThatImplementsNothingGivesTheLanguagesErrorForEachNewOperation) {
  Types types;
  struct Case {
    const char * source;
    GLTANG_ErrorKind error;
  } cases[] = {
    {"x = 0; for (i : plain()) { x = 1; }", GLTANG_ERROR_NOT_SUPPORTED},
    {"plain()(1, 2);", GLTANG_ERROR_INVALID_FUNCTION_CALL},
    {"plain() as int;", GLTANG_ERROR_NOT_SUPPORTED},
    {"plain() as float;", GLTANG_ERROR_NOT_SUPPORTED},
    {"plain() as string;", GLTANG_ERROR_NOT_SUPPORTED},
  };
  for (const Case & c : cases) {
    Out o = run(std::string(PRELUDE) + c.source, types);
    ASSERT_TRUE(o.finished) << c.source;
    EXPECT_EQ(o.kind, GLTANG_KIND_ERROR) << c.source << " gave " << o.described;
    EXPECT_EQ(o.error, c.error) << c.source << " gave " << o.described;
  }
  Out o = run(PRELUDE "p = plain(); [p ? 1 : 2, !p, p && 7, p || 7, p as bool];", types);
  EXPECT_EQ(o.described, "[1, false, 7, Custom: plain, true]") << "truthy by default";
  o = run(PRELUDE "print(\"a\"); print(plain()); print(\"b\"); print([plain()]);", types);
  EXPECT_EQ(o.raw, "ab[Custom: plain]") << "print appends nothing; an element shows the name";
}

TEST(TypeOps, ATypeThatImplementsEverythingIsReachedWithTheDocumentedOperands) {
  Types types;
  Out o = run(PRELUDE
              "a = all(); n = 0; for (x : a) { n += x; } "
              "r = a(1, \"s\", null, [1], 2.5); "
              "c = [a as int, a as float, a as bool, a as string]; "
              "t = [a ? 1 : 2, !a, a && 1, a || 2]; "
              "print(a); print([a]); "
              "[n, r, c, t];",
              types);
  ASSERT_TRUE(o.finished);
  Log expected = {
    "all begin",
    "all call argc=5 kinds=2,4,0,5,3, self_custom=1",
    "all cast 0", "all cast 1", "all cast 2", "all cast 3",
    "all truthy", "all truthy", "all truthy", "all truthy",
    "all print",
    "all to_string",
  };
  EXPECT_EQ(o.log, expected);
  // as bool is the cast's answer (true), not the truthiness (false).
  EXPECT_EQ(o.described, "[3, 77, [5, 2.5, true, 5], [2, true, Custom: all, 2]]");
  EXPECT_EQ(o.raw, "P[S]");
}

TEST(TypeOps, ADeclineFallsToTheDefaultOfAnEntryThatIsAbsent) {
  Types types;
  Out o = run(PRELUDE
              "d = decliner(); x = 0; for (i : d) { x = 1; } "
              "[x, d(1), d as int, d as float, d as string, d as bool, d ? 1 : 2, !d, d && 3, d || 3, [d]];",
              types);
  ASSERT_TRUE(o.finished);
  // The loop's value is Not supported and the program goes on (the loop is a statement).
  EXPECT_EQ(o.described,
            "[0, Error: Invalid function call, Error: Not supported, Error: Not supported, Error: Not supported, true, 1, false, 3, Custom: decliner, [Custom: decliner]]");
  // Each operation was asked once; a decline made no value.
  // as string asks the cast first, then to_string; as bool asks the cast, then the truthiness.
  Log asked = {"dec begin", "dec call", "dec cast", "dec cast", "dec cast", "dec text", "dec cast", "dec truthy", "dec truthy",
               "dec truthy", "dec truthy", "dec truthy"};
  EXPECT_EQ(o.log, asked) << "the element rendering of the result is the host's describe, which runs no callback";
}

// ---------------------------------------------------------------------------
// Iteration
// ---------------------------------------------------------------------------

TEST(TypeOps, ForOverAHostCollectionIteratesItsItems) {
  Types types;
  Out o = run(PRELUDE "s = 0; log = []; for (x : coll(3)) { s += x; log += [x]; } [s, log];", types);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.described, "[6, [1, 2, 3]]");
  EXPECT_EQ(o.log, (Log{"coll begin", "coll next pos=0", "coll next pos=1", "coll next pos=2", "coll next pos=3"}));
  // The loop's value is null, as an array loop's is (the end of the iteration is not an error).
  o = run(PRELUDE "for (x : coll(3)) { 1; }", types);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.kind, GLTANG_KIND_NULL) << o.described;
  // The body runs for 1 and 2, and for nothing when the collection is empty.
  o = run(PRELUDE "s = 0; for (x : coll(2)) { s = s * 10 + x; } e = 0; for (x : coll(0)) { e = 1; } [s, e];", types);
  EXPECT_EQ(o.described, "[12, 0]");
}

TEST(TypeOps, TheIteratorMayBeTheInstanceItself) {
  Types types;
  Out o = run(PRELUDE "s = 0; for (x : coll(3, 4)) { s += x; } s;", types);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.integer, 6);
}

TEST(TypeOps, BeginMayAnswerAnArray) {
  Types types;
  Out o = run(PRELUDE "s = 0; for (x : coll(0, 5)) { s = s * 10 + x; } s;", types);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.integer, 78);
  EXPECT_EQ(o.log, (Log{"coll begin"}));
}

TEST(TypeOps, BeginAnsweringAnythingElseOrNothingIsNotSupportedAsTheLoopsValue) {
  Types types;
  // 6: an integer; 7: a decline; 9: an instance with no iterator_next; plain: no entry.
  for (const char * source : {"for (x : coll(3, 6)) { 1; }", "for (x : coll(3, 7)) { 1; }", "for (x : coll(3, 9)) { 1; }", "for (x : plain()) { 1; }"}) {
    Out o = run(std::string(PRELUDE) + source, types);
    ASSERT_TRUE(o.finished) << source;
    EXPECT_EQ(o.kind, GLTANG_KIND_ERROR) << source << ": " << o.described;
    EXPECT_EQ(o.error, GLTANG_ERROR_NOT_SUPPORTED) << source;
  }
  // The loop's value is an error, and the body did not run.
  Out o = run(PRELUDE "x = 0; for (i : coll(3, 6)) { x = 1; } x;", types);
  EXPECT_EQ(o.integer, 0);
}

TEST(TypeOps, ABeginThatErrorsIsTheLoopsValue) {
  Types types;
  Out o = run(PRELUDE "for (x : coll(3, 8)) { 1; }", types);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.error, GLTANG_ERROR_DIVIDE_BY_ZERO);
}

TEST(TypeOps, ANextThatErrorsEndsTheLoopAndTheErrorIsItsValue) {
  Types types;
  Out o = run(PRELUDE "s = 0; for (x : coll(2, 2)) { s += x; }", types);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.kind, GLTANG_KIND_ERROR) << o.described;
  EXPECT_EQ(o.error, GLTANG_ERROR_MODULO_BY_ZERO);
  o = run(PRELUDE "s = 0; for (x : coll(2, 2)) { s += x; } s;", types);
  EXPECT_EQ(o.integer, 3) << "the items before the error were seen, and the program went on";
}

TEST(TypeOps, ANextThatDeclinesEndsTheLoopWithNotSupported) {
  Types types;
  Out o = run(PRELUDE "s = 0; for (x : coll(5, 3)) { s += x; }", types);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.kind, GLTANG_KIND_ERROR) << o.described;
  EXPECT_EQ(o.error, GLTANG_ERROR_NOT_SUPPORTED);
  o = run(PRELUDE "s = 0; for (x : coll(5, 3)) { s += x; } s;", types);
  EXPECT_EQ(o.integer, 1);
}

TEST(TypeOps, ABreakAndNestedLoopsOverHostCollectionsAndArrays) {
  Types types;
  Out o = run(PRELUDE
              "s = 0; for (x : coll(4)) { for (y : coll(3)) { if (y == 3) { break; } s += x * y; } for (z : [1, 2]) { s += z; } } s;",
              types);
  ASSERT_TRUE(o.finished);
  // sum over x in 1..4 of (x*1 + x*2 + 1 + 2)
  EXPECT_EQ(o.integer, (1 + 2 + 3 + 4) * 3 + 4 * 3);
}

TEST(TypeOps, EachLoopOverASharedCollectionHasAnIteratorOfItsOwn) {
  Types types;
  Out o = run(PRELUDE "c = coll(3); n = 0; for (x : c) { for (y : c) { n += 1; } } n;", types);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.integer, 9) << "begin makes an iterator per loop: the collection's own state is not the loop's";
}

TEST(TypeOps, AnIteratorThatAllocatesIsKeptAliveInTheFramesSlotUnderTortureAndVerify) {
  Types types;
  Out o = run(PRELUDE "s = \"\"; for (x : coll(6, 1)) { s += x + \",\"; } s;", types, forced_torture());
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.described, "item1,item2,item3,item4,item5,item6,");
  // Collected between every step by the program itself, too.
  o = run(PRELUDE "n = 0; for (x : coll(20, 1)) { t.collect(); n += 1; } n;", types, forced_torture());
  EXPECT_EQ(o.integer, 20);
}

// ---------------------------------------------------------------------------
// Calls
// ---------------------------------------------------------------------------

TEST(TypeOps, ACallableObjectIsCalledWithItsArgumentsAsHandles) {
  Types types;
  Out o = run(PRELUDE "c = callee(); [c(), c(5), c(5, \"a\"), c(1, 2, 3, 4)];", types);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.described, "[0, 1005, 2005, 4001]");
  EXPECT_EQ(o.log, (Log{"callee call argc=0", "callee call argc=1", "callee call argc=2", "callee call argc=4"}));
}

TEST(TypeOps, AManyArgumentCallIsCopiedToTheHeapAsANativesIs) {
  Types types;
  std::string args;
  for (int i = 0; i < 40; ++i) {
    args += (i ? ", " : "") + std::to_string(i + 1);
  }
  Out o = run(PRELUDE "c = callee(); c(" + args + ");", types);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.integer, 40 * 1000 + 1);
}

TEST(TypeOps, ACallWithNoEntryOrADeclineIsInvalidFunctionCallAndAnErrorIsTheValue) {
  Types types;
  Out o = run(PRELUDE "callee(1)(1);", types);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.error, GLTANG_ERROR_INVALID_FUNCTION_CALL) << "mode 1 declines";
  o = run(PRELUDE "callee(2)(1);", types);
  EXPECT_EQ(o.error, GLTANG_ERROR_NOT_A_NUMBER);
  o = run(PRELUDE "callee(3)(1);", types);
  EXPECT_TRUE(o.finished);
  EXPECT_EQ(o.kind, GLTANG_KIND_NULL) << "no answer is null";
  o = run(PRELUDE "plain()(1);", types);
  EXPECT_EQ(o.error, GLTANG_ERROR_INVALID_FUNCTION_CALL);
}

TEST(TypeOps, ACallableInstanceIsAValueNotAFunction) {
  Types types;
  // It is not accepted where only a function is: as a template result or a call of a non-callable.
  Out o = run(PRELUDE "c = callee(); f = c; [f(7), c(7) == f(7)];", types);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.described, "[1007, true]");
}

// ---------------------------------------------------------------------------
// Casts
// ---------------------------------------------------------------------------

TEST(TypeOps, ACastIsAskedFirstForAllFourTargets) {
  Types types;
  Out o = run(PRELUDE "c = caster(0); [c as int, c as float, c as bool, c as string];", types);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.described, "[41, 2.5, false, castS]");
  EXPECT_EQ(o.log, (Log{"caster cast 0", "caster cast 1", "caster cast 2", "caster cast 3"}));
}

TEST(TypeOps, ADeclinedCastFallsToTheDefaults) {
  Types types;
  // Mode 1 declines bool and string: bool is the truthiness (true: the type has no truthy), string
  // is Not supported (no to_string).
  Out o = run(PRELUDE "c = caster(1); [c as int, c as float, c as bool, c as string];", types);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.described, "[41, 2.5, true, Error: Not supported]");
  // Mode 2 declines all four.
  o = run(PRELUDE "c = caster(2); [c as int, c as float, c as bool, c as string];", types);
  EXPECT_EQ(o.described, "[Error: Not supported, Error: Not supported, true, Error: Not supported]");
  // An error answer is the value.
  o = run(PRELUDE "caster(3) as int;", types);
  EXPECT_EQ(o.error, GLTANG_ERROR_NOT_A_NUMBER);
}

TEST(TypeOps, ADeclinedCastToStringFallsToToStringAndToBoolToTruthy) {
  Types types;
  // texty has to_string and no cast; truthy has truthy and no cast.
  Out o = run(PRELUDE "[texty(0) as string, truthy(0) as bool, truthy(1) as bool, truthy(2) as bool];", types);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.described, "[T<&>, false, true, true]");
}

// ---------------------------------------------------------------------------
// Truthiness
// ---------------------------------------------------------------------------

TEST(TypeOps, EveryTruthTestAsksTheType) {
  Types types;
  // truthy(0) is false, truthy(1) true, truthy(2) declines (true).
  Out o = run(PRELUDE
              "f = truthy(0); t1 = truthy(1); d = truthy(2); "
              "[f ? 1 : 2, t1 ? 1 : 2, d ? 1 : 2, !f, !t1, !d, f && 9, t1 && 9, f || 9, t1 || 9, f as bool, t1 as bool, d as bool];",
              types);
  ASSERT_TRUE(o.finished);
  // && and || give the operand when it decides: f && 9 is f (an instance), t1 || 9 is t1.
  EXPECT_EQ(o.described,
            "[2, 1, 1, true, false, false, Custom: truthy, 9, 9, Custom: truthy, false, true, true]");
}

TEST(TypeOps, TruthTestsInLoopsAndConditionals) {
  Types types;
  Out o = run(PRELUDE
              "f = truthy(0); n = 0; i = 0; while (f || i < 3) { i += 1; n += 1; } "
              "w = truthy(1); k = 0; while (w && k < 4) { k += 1; } "
              "if (f) { n += 100; } else { n += 1000; } if (w) { n += 10000; } "
              "a = 0; do { a += 1; if (a > 5) { break; } } while (f); "
              "b = 0; do { b += 1; if (b > 3) { break; } } while (w); "
              "[n, k, a, b];",
              types);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.described, "[11003, 4, 1, 4]") << "a do-while takes its test from JMP_TRUE";
}

TEST(TypeOps, ATruthyThatErrorsIsTheExpressionsValue) {
  Types types;
  Out o = run(PRELUDE "e = truthy(3); [!e, e && 1, e || 5, e as bool, e ? 1 : 2];", types);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.described, "[Error: Divide by zero, Error: Divide by zero, 5, Error: Divide by zero, 2]");
}

TEST(TypeOps, ATruthyIsAGcPointAndTheInstanceSurvivesIt) {
  Types types;
  Out o = run(PRELUDE "t1 = truthy(1); n = 0; i = 0; while (i < 30) { if (t1 && !(t1 as bool == false)) { n += 1; } i += 1; } n;", types,
              forced_torture());
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.integer, 30);
}

// ---------------------------------------------------------------------------
// Print and to-string
// ---------------------------------------------------------------------------

TEST(TypeOps, PrintAppendsTheAnswerWithItsOwnTag) {
  Types types;
  Out o = run(PRELUDE "print(texty(0));", types);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.raw, "<b>&</b>");
  EXPECT_EQ(o.rendered, "&lt;b&gt;&amp;&lt;/b&gt;") << "the tag is kept: a segment tagged HTML is escaped for HTML on output";
  o = run(PRELUDE "print(texty(6));", types);
  EXPECT_EQ(o.raw, "a<<i>");
  EXPECT_EQ(o.rendered, "a<&lt;i&gt;") << "escaped per segment: the trusted segment is raw, the HTML one is escaped";
}

TEST(TypeOps, PrintFallsBackToToStringAndThenToNothing) {
  Types types;
  Out o = run(PRELUDE "print(texty(2)); print(\"|\"); print(texty(7)); print(\"|\"); print(plain());", types);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.raw, "T<&>|T<&>|");
  EXPECT_EQ(o.rendered, "T<&>|T<&>|") << "to_string's text is a trusted string";
}

TEST(TypeOps, ToStringIsTheElementTextAndTheCastAndConcatenationOfAContainer) {
  Types types;
  Out o = run(PRELUDE "x = texty(0); [[x] as string, \"a\" + [x]];", types);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.described, "[[T<&>], a[T<&>]]");
  o = run(PRELUDE "print([texty(0), texty(1), plain()]); print({k: texty(2)});", types);
  EXPECT_EQ(o.raw, "[T<&>, Custom: texty, Custom: plain]{\"k\": T<&>}") << "an element uses to_string, not print; a type without it shows its name";
  // Only print: an element has no to_string, and shows the name.
  o = run(PRELUDE "print([texty(1)]);", types);
  EXPECT_EQ(o.raw, "[Custom: texty]");
}

TEST(TypeOps, AnAnswerThatIsNotAStringIsHostFunctionFailedAndNothingPartialIsAppended) {
  Types types;
  // print answers an integer / an error; to_string answers an integer / an error.
  Out o = run(PRELUDE "print(\"a\"); r = print(texty(3)); print(\"b\"); r;", types);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.raw, "ab");
  EXPECT_EQ(o.error, GLTANG_ERROR_HOST_FAILED);
  o = run(PRELUDE "print(\"a\"); print([1, texty(4), 3]); print(\"b\");", types);
  EXPECT_EQ(o.raw, "ab") << "the whole print failed: the part of the array before the element is not left behind";
  o = run(PRELUDE "texty(4) as string;", types);
  EXPECT_EQ(o.error, GLTANG_ERROR_HOST_FAILED);
  o = run(PRELUDE "texty(5) as int;", types);
  EXPECT_EQ(o.kind, GLTANG_KIND_ERROR) << "texty has no cast";
  o = run(PRELUDE "print(\"a\"); r = print(texty(5)); print(\"b\"); r;", types);
  EXPECT_EQ(o.raw, "ab");
  EXPECT_EQ(o.error, GLTANG_ERROR_NOT_A_NUMBER) << "an error answer is the print's value";
  o = run(PRELUDE "texty(8) as string;", types);
  EXPECT_EQ(o.error, GLTANG_ERROR_NOT_A_NUMBER);
  o = run(PRELUDE "\"x\" + [texty(8)];", types);
  EXPECT_EQ(o.error, GLTANG_ERROR_NOT_A_NUMBER) << "a concatenation of a container that fails";
}

TEST(TypeOps, TheRenderersCallsAreGcPointsAndTheContainerIsReadAgainAfterEach) {
  Types types;
  Out o = run(PRELUDE "a = [texty(2), 1, texty(2), [texty(2)]]; m = {a: texty(2), b: [texty(2), 2]}; print(a); print(m); (a as string) + (m as string);", types,
              forced_torture());
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.raw, "[T<&>, 1, T<&>, [T<&>]]{\"a\": T<&>, \"b\": [T<&>, 2]}");
}

TEST(TypeOps, AnInspectorsBufferRenderNeverRunsTheHost) {
  Types types;
  // The result of the program is an array holding an instance: describing it is a buffer render.
  Out o = run(PRELUDE "[texty(0)];", types);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.described, "[Custom: texty]");
  EXPECT_TRUE(o.log.empty()) << "no callback ran for the host's describe";
}

// ---------------------------------------------------------------------------
// An instance that stands for an error
// ---------------------------------------------------------------------------

TEST(TypeOps, AMarkedInstanceIsFalsyAndTheTypeIsNotAsked) {
  Types types;
  Out o = run(PRELUDE "e = truthy(1); m = mark(e); [marked(e), e ? 1 : 2, !e, e && 5, e || 5, e as bool];", types);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.described, "[true, 2, true, Custom: truthy, 5, false]");
  EXPECT_TRUE(o.log.empty()) << "the type's truthy is never asked of an error instance";
  // A mark can be cleared.
  o = run(PRELUDE "e = truthy(1); mark(e); mark(e, false); [marked(e), e ? 1 : 2];", types);
  EXPECT_EQ(o.described, "[false, 1]");
}

TEST(TypeOps, AMarkedInstanceContaminatesWhereAnEngineErrorDoes) {
  Types types;
  Out o = run(PRELUDE "e = plain(); mark(e); x = e; y = e + 1; z = 2 * e; w = \"s\" + e; "
                      "function f(v) { return v; } [x == e, y == e, z == e, w == e, f(e) == e, (e < 1) == e, f(e) == e];",
              types);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.described, "[true, true, true, true, true, true, true]");
  // A function's result and a statement's value.
  o = run(PRELUDE "e = plain(); mark(e); function f(v) { return v; } f(e);", types);
  EXPECT_EQ(o.kind, GLTANG_KIND_CUSTOM);
}

TEST(TypeOps, AMarkedInstanceIsNotAnEngineError) {
  Types types;
  // Never listed, never fires the hook (halt-on-error would stop the run), has no error kind.
  Out o = run(PRELUDE "e = plain(); mark(e); a = e + 1; b = e; q = e ? 1 : 2; r = !e; e; 7;", types, Config(), [](Context & c, Types &) {
    ASSERT_EQ(gltang_execution_set_log_all_errors(c.execution, true), GLTANG_OK);
    ASSERT_EQ(gltang_execution_set_halt_on_error(c.execution, true), GLTANG_OK);
  });
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.integer, 7);
  EXPECT_EQ(o.errors_listed, 0u);
  o = run(PRELUDE "e = plain(); mark(e); e;", types);
  EXPECT_EQ(o.kind, GLTANG_KIND_CUSTOM) << "its kind is custom, not error";
  EXPECT_EQ(o.error, GLTANG_ERROR_KIND_COUNT);
  // A real error, in the same run, is still listed.
  o = run(PRELUDE "e = plain(); mark(e); a = e + 1; b = 1 / 0; 7;", types, Config(), [](Context & c, Types &) {
    ASSERT_EQ(gltang_execution_set_log_all_errors(c.execution, true), GLTANG_OK);
  });
  EXPECT_EQ(o.errors_listed, 1u);
}

TEST(TypeOps, AnEqualThatAnswersAMarkedInstanceIsTheComparisonsValueAtTheTopAndInAContainer) {
  Types types;
  Out o = run(PRELUDE "a = eqmark(); b = eqmark(); r = a == b; q = a != b; c = [a] == [b]; [marked(r), marked(q), marked(c), r ? 1 : 2, c ? 1 : 2];", types);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.described, "[true, true, true, 2, 2]") << "the marked instance is the value of ==, of != and of the comparison of two arrays, and it is falsy";
  // A marked element of a container is itself, and the container's own comparison is identity.
  o = run(PRELUDE "e = plain(); mark(e); a = [e]; b = [e]; [a == b, marked(a[0])];", types);
  EXPECT_EQ(o.described, "[true, true]");
}

TEST(TypeOps, TheFlagIsRefusedOnAnythingButACustomValue) {
  Types types;
  Out o = run(PRELUDE "[mark(1), mark(\"s\"), mark([1]), mark(null), marked(1), marked(\"s\")];", types);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.described, "[0, 0, 0, 0, false, false]");
}

// ---------------------------------------------------------------------------
// Attributes on built-in kinds
// ---------------------------------------------------------------------------

TEST(TypeOps, AHostAttributeIsReadOnAString) {
  Types types;
  Out o = run(PRELUDE "s = \"abc\"; t2 = \"x y\"; [s.slug, t2.slug, s.slug.slug];", types);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.described, "[slug:abc, slug:x y, slug:slug:abc]");
}

TEST(TypeOps, ABuiltInNameAlwaysWins) {
  Types types;
  // `html`, `length` on a string, `size` on an array and the generator's `next_int` are registered
  // above with an answer "WRONG" that must never be seen.
  Out o = run(PRELUDE "s = \"a<b\"; a = [1, 2, 3]; use random; r = random.seeded(1); [s.length, s.html, a.size, r.next_int >= 0];", types);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.described, "[3, a<b, 3, true]");
  for (const std::string & line : o.log) {
    EXPECT_EQ(line.find("wrong"), std::string::npos) << line;
  }
  EXPECT_TRUE(o.log.empty()) << "the host function was never reached for a built-in name";
}

TEST(TypeOps, HostAttributesOnEveryKindTheyMayBeAddedTo) {
  Types types;
  Out o = run(PRELUDE "use random; function g() { return 1; } r = random.seeded(2); "
                      "a = [1, 2]; n = 21; f = 2.5; b = true; [a.extra, n.double, f.kind, b.kind, g.kind, r.kind];",
              types);
  ASSERT_TRUE(o.finished);
  // The kind numbers: float 3, bool 1, function 7, rng 10.
  EXPECT_EQ(o.described, "[102, 42, 3, 1, 7, 10]");
}

TEST(TypeOps, ADeclinedOrUnknownHostAttributeIsNotImplemented) {
  Types types;
  Out o = run(PRELUDE "s = \"s\"; s.nope;", types);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.error, GLTANG_ERROR_NOT_IMPLEMENTED) << "a decline";
  o = run(PRELUDE "s = \"s\"; s.missing;", types);
  EXPECT_EQ(o.error, GLTANG_ERROR_NOT_IMPLEMENTED) << "no such attribute";
  o = run(PRELUDE "s = \"s\"; s.boom;", types);
  EXPECT_EQ(o.error, GLTANG_ERROR_NOT_A_NUMBER) << "an error answer is the value";
  o = run(PRELUDE "n = 1; n.missing;", types);
  EXPECT_EQ(o.error, GLTANG_ERROR_NOT_IMPLEMENTED);
  o = run(PRELUDE "n = null; n.double;", types);
  EXPECT_EQ(o.error, GLTANG_ERROR_NOT_IMPLEMENTED) << "null takes none";
  o = run(PRELUDE "n = 1; n.slug;", types);
  EXPECT_EQ(o.error, GLTANG_ERROR_NOT_IMPLEMENTED) << "an attribute is of one kind";
}

TEST(TypeOps, AMapsNamesAreItsKeysAndALibrarysAreItsMembers) {
  Types types;
  GLTANG_TypeRegistry * r = nullptr;
  ASSERT_EQ(gltang_type_registry_create(&r), GLTANG_OK);
  EXPECT_EQ(gltang_type_registry_add_attribute(r, GLTANG_KIND_MAP, "slug", attr_slug, nullptr), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_type_registry_add_attribute(r, GLTANG_KIND_LIBRARY, "slug", attr_slug, nullptr), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_type_registry_add_attribute(r, GLTANG_KIND_CUSTOM, "slug", attr_slug, nullptr), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_type_registry_add_attribute(r, GLTANG_KIND_NULL, "slug", attr_slug, nullptr), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_type_registry_add_attribute(r, GLTANG_KIND_ERROR, "slug", attr_slug, nullptr), GLTANG_ERR_INVALID);
  gltang_type_registry_release(r);
  // And a map's key is still its key.
  Out o = run(PRELUDE "m = {slug: 5, size: 6}; [m.slug, m.size, m.other];", types);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.described, "[5, 6, null]");
}

TEST(TypeOps, RegisteringAnAttributeIsCheckedAndAClashIsRefused) {
  GLTANG_TypeRegistry * r = nullptr;
  ASSERT_EQ(gltang_type_registry_create(&r), GLTANG_OK);
  EXPECT_EQ(gltang_type_registry_add_attribute(nullptr, GLTANG_KIND_STRING, "a", attr_slug, nullptr), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_type_registry_add_attribute(r, GLTANG_KIND_STRING, nullptr, attr_slug, nullptr), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_type_registry_add_attribute(r, GLTANG_KIND_STRING, "", attr_slug, nullptr), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_type_registry_add_attribute(r, GLTANG_KIND_STRING, "a b", attr_slug, nullptr), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_type_registry_add_attribute(r, GLTANG_KIND_STRING, "9a", attr_slug, nullptr), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_type_registry_add_attribute(r, GLTANG_KIND_STRING, "a", nullptr, nullptr), GLTANG_ERR_INVALID);
  static const std::string long65(65, 'n');
  static const std::string long64(64, 'n');
  EXPECT_EQ(gltang_type_registry_add_attribute(r, GLTANG_KIND_STRING, long65.c_str(), attr_slug, nullptr), GLTANG_ERR_INVALID);
  EXPECT_EQ(gltang_type_registry_add_attribute(r, GLTANG_KIND_STRING, long64.c_str(), attr_slug, nullptr), GLTANG_OK);
  EXPECT_EQ(gltang_type_registry_add_attribute(r, GLTANG_KIND_STRING, "slug", attr_slug, nullptr), GLTANG_OK);
  EXPECT_EQ(gltang_type_registry_add_attribute(r, GLTANG_KIND_STRING, "slug", attr_slug, nullptr), GLTANG_ERR_INVALID) << "a duplicate in one registry";
  EXPECT_EQ(gltang_type_registry_add_attribute(r, GLTANG_KIND_ARRAY, "slug", attr_slug, nullptr), GLTANG_OK) << "another kind, same name";
  // Many, so the table grows.
  for (int i = 0; i < 40; ++i) {
    EXPECT_EQ(gltang_type_registry_add_attribute(r, GLTANG_KIND_INTEGER, ("n" + std::to_string(i)).c_str(), attr_slug, nullptr), GLTANG_OK);
  }
  // Attaching seals it, and a sealed registry refuses an attribute.
  Compiled compiled("1;");
  Context context(compiled.program);
  ASSERT_EQ(gltang_execution_attach_types(context.execution, r), GLTANG_OK);
  EXPECT_EQ(gltang_type_registry_add_attribute(r, GLTANG_KIND_STRING, "late", attr_slug, nullptr), GLTANG_ERR_INVALID);
  // A second registry with the same kind and name cannot be attached beside it.
  GLTANG_TypeRegistry * r2 = nullptr;
  ASSERT_EQ(gltang_type_registry_create(&r2), GLTANG_OK);
  EXPECT_EQ(gltang_type_registry_add_attribute(r2, GLTANG_KIND_STRING, "slug", attr_slug, nullptr), GLTANG_OK);
  EXPECT_EQ(gltang_execution_attach_types(context.execution, r2), GLTANG_ERR_INVALID);
  GLTANG_TypeRegistry * r3 = nullptr;
  ASSERT_EQ(gltang_type_registry_create(&r3), GLTANG_OK);
  EXPECT_EQ(gltang_type_registry_add_attribute(r3, GLTANG_KIND_FLOAT, "slug", attr_slug, nullptr), GLTANG_OK);
  EXPECT_EQ(gltang_execution_attach_types(context.execution, r3), GLTANG_OK) << "the same name on another kind does not clash";
  EXPECT_EQ(gltang_execution_attach_types(context.execution, r), GLTANG_OK) << "attaching the same registry again is a no-op";
  gltang_type_registry_release(r);
  gltang_type_registry_release(r2);
  gltang_type_registry_release(r3);
}

// ---------------------------------------------------------------------------
// Malformed answers
// ---------------------------------------------------------------------------

TEST(TypeOps, DoneFromAnythingButAnIteratorsNextIsHostFunctionFailed) {
  Types types;
  struct Case {
    const char * source;
  } cases[] = {
    {"b = bad(); b + 1;"},
    {"b = bad(); b(1);"},
    {"b = bad(); b as int;"},
    {"b = bad(); b as string;"},
    {"b = bad(); r = print(b); r;"},
    {"b = bad(); [b] as string;"},
    {"b = bad(); !b;"},
    {"b = bad(); x = 0; for (i : b) { x = 1; }"},
  };
  for (const Case & c : cases) {
    Out o = run(std::string(PRELUDE) + c.source, types);
    ASSERT_TRUE(o.finished) << c.source;
    EXPECT_EQ(o.kind, GLTANG_KIND_ERROR) << c.source << ": " << o.described;
    EXPECT_EQ(o.error, GLTANG_ERROR_HOST_FAILED) << c.source;
  }
  // `b as bool` and the truthiness: the cast's DONE is the failure; and `b ? 1 : 2` takes an
  // error answer as falsy.
  Out o = run(PRELUDE "b = bad(); [b ? 1 : 2];", types);
  EXPECT_EQ(o.described, "[2]");
}

TEST(TypeOps, ABeginWhoseIteratorsNextAnswersDoneAtOnceIsAnEmptyLoop) {
  Types types;
  Out o = run(PRELUDE "x = 0; for (i : coll(0, 4)) { x = 1; } x;", types);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.integer, 0);
}

// ---------------------------------------------------------------------------
// What a decline and a miss cost
// ---------------------------------------------------------------------------

TEST(TypeOps, TheDeclinePathOfEveryNewOperationAllocatesWhatNoEntryDid) {
  Types types;
  // For each operation, the delta in heap objects and in allocator calls across the operation on
  // a type that has no entry and on one that declines. A decline allocates nothing of its own (it
  // makes no value and logs no error), so the two are equal; and neither differs from the same
  // operation before the hook existed, which the first column pins.
  const char * ops[] = {
    "x = (%s) ? 1 : 2;",
    "x = !(%s);",
    "x = (%s) && 1;",
    "x = (%s) as bool;",
    "x = (%s) as int;",
    "x = (%s)(1);",
    "x = (%s) as string;",
    "for (i : (%s)) { x = 1; }",
    "x = [(%s)];",
  };
  for (const char * op : ops) {
    std::string deltas[2];
    int which = 0;
    for (const char * make : {"plain()", "decliner()"}) {
      std::string statement = op;
      statement.replace(statement.find("%s"), 2, "o");
      Out o = run(std::string(PRELUDE) + "o = " + make + "; allocs = t.allocs; mallocs = t.mallocs; " + statement + statement +
                      " a0 = allocs(); m0 = mallocs(); " + statement + " a1 = allocs(); m1 = mallocs(); [a1 - a0, m1 - m0];",
                  types, no_instruments());
      ASSERT_TRUE(o.finished) << op;
      deltas[which++] = o.described;
    }
    EXPECT_EQ(deltas[0], deltas[1]) << op;
  }
}

TEST(TypeOps, ABuiltInAttributeReadAllocatesWhatItDidWithNoRegistryAttached) {
  Types types;
  const std::string program = PRELUDE
      "allocs = t.allocs; mallocs = t.mallocs; s = \"abc\"; a = [1, 2]; "
      "x = s.length; y = s.html; z = a.size; "
      "a0 = allocs(); m0 = mallocs(); x = s.length; y = s.html; z = a.size; a1 = allocs(); m1 = mallocs(); [a1 - a0, m1 - m0];";
  Out with = run(program, types, no_instruments());
  Out without = run(program, types, no_instruments(), nullptr, false);
  ASSERT_TRUE(with.finished);
  ASSERT_TRUE(without.finished);
  EXPECT_EQ(with.described, without.described);
}

TEST(TypeOps, AHostAttributeOnlyCostsWhereTheBuiltInArmWouldHaveFailed) {
  Types types;
  Out o = run(PRELUDE "s = \"abc\"; n = 0; i = 0; while (i < 50) { n += s.length; i += 1; } n;", types);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.integer, 150);
  EXPECT_TRUE(o.log.empty());
}

// ---------------------------------------------------------------------------
// Compiled code, and the collector
// ---------------------------------------------------------------------------

namespace {

const char * const kDifferentialPrograms[] = {
    // Truth tests, casts, calls and iteration with a custom operand in functions that are hot.
    PRELUDE "function tt(x, n) { s = 0; i = 0; while (i < n) { if (x) { s += 1; } if (!x) { s += 10; } s += (x && 1) == 1 ? 100 : 0; i += 1; } return s; } "
            "[tt(truthy(1), 20), tt(truthy(0), 20), tt(truthy(2), 20), tt(plain(), 20)];",
    PRELUDE "function c(x, n) { s = 0; i = 0; while (i < n) { s += x(i); i += 1; } return s; } [c(callee(0), 30)];",
    PRELUDE "function k(x, n) { s = 0; i = 0; while (i < n) { s += (x as int); i += 1; } return s; } [k(caster(0), 25)];",
    PRELUDE "function g(x, n) { s = \"\"; i = 0; while (i < n) { s += (x as string); i += 1; } return s; } [g(texty(2), 5), g(caster(0), 3)];",
    PRELUDE "function p(x, n) { i = 0; while (i < n) { print(x); print([x]); i += 1; } } p(texty(0), 4); p(texty(2), 3);",
    PRELUDE "function it(x) { s = 0; for (v : x) { s += v; } return s; } [it(coll(5)), it(coll(0, 5)), it([1, 2, 3])];",
    PRELUDE "function a(s, n) { r = \"\"; i = 0; while (i < n) { r = s.slug; i += 1; } return r; } [a(\"x\", 30), a(\"y\", 30)];",
    PRELUDE "function e(x, n) { s = 0; i = 0; while (i < n) { y = x + 1; if (y) { s += 1; } i += 1; } return s; } m = plain(); mark(m); [e(m, 5), e(1, 5)];",
    PRELUDE "function v(m, n) { s = 0; i = 0; while (i < n) { if (m) { s += 1; } else { s += 2; } i += 1; } return s; } m = plain(); mark(m); [v(m, 30), v(plain(), 30)];",
};

}  // namespace

TEST(TypeOps, CompiledAndInterpretedRunsGiveTheSameAnswers) {
  Types types;
  for (const char * source : kDifferentialPrograms) {
    Out reference;
    bool first = true;
    for (long threshold : {0L, 1L, 2L}) {
      Config config;
      config.jit_threshold = threshold;
      Out o = run(source, types, config);
      ASSERT_TRUE(o.finished) << source;
      if (first) {
        reference = o;
        first = false;
        EXPECT_NE(o.kind, GLTANG_KIND_ERROR) << source << ": " << o.described;
        continue;
      }
      if (threshold == 1) {
        EXPECT_COMPILED(o, source) << ": the functions were compiled, so this compares the two tiers";
      }
      EXPECT_EQ(o.described, reference.described) << source << " at threshold " << threshold;
      EXPECT_EQ(o.raw, reference.raw) << source << " at threshold " << threshold;
      EXPECT_EQ(o.fuel, reference.fuel) << source << " at threshold " << threshold << ": fuel parity with the interpreter";
      EXPECT_EQ(o.log, reference.log) << source << " at threshold " << threshold;
    }
  }
}

TEST(TypeOps, CompiledAndInterpretedRunsAgreeUnderATightNativeDepthBudget) {
  Types types;
  // One unit of native depth: a native runs at the top level. From a helper that compiled code
  // called, the units its open records hold are handed back for the callback (as for a native),
  // so the callback is refused in neither tier.
  for (const char * source : {kDifferentialPrograms[2], kDifferentialPrograms[3], kDifferentialPrograms[4]}) {
    Out reference;
    bool first = true;
    for (long threshold : {0L, 1L}) {
      Config config;
      config.jit_threshold = threshold;
      config.native_depth = 1;
      Out o = run(source, types, config);
      ASSERT_TRUE(o.finished) << source;
      if (first) {
        reference = o;
        first = false;
        EXPECT_NE(o.kind, GLTANG_KIND_ERROR) << source << ": " << o.described;
        continue;
      }
      EXPECT_COMPILED(o, source);
      EXPECT_EQ(o.described, reference.described) << source;
      EXPECT_EQ(o.raw, reference.raw) << source;
      EXPECT_EQ(o.fuel, reference.fuel) << source;
    }
  }
}

TEST(TypeOps, EveryOperationUnderTortureAndVerifyKeepsItsOperandsAlive) {
  Types types;
  for (const char * source : kDifferentialPrograms) {
    Out plain_run = run(source, types, no_instruments());
    Out torture = run(source, types, forced_torture());
    ASSERT_TRUE(torture.finished) << source;
    EXPECT_EQ(torture.described, plain_run.described) << source;
    EXPECT_EQ(torture.raw, plain_run.raw) << source;
  }
}

TEST(TypeOps, TheNewOperationsAreChargedAsANativeCallPlusTheBytesTheyAnswer) {
  Types types;
  auto fuel = [&](const std::string & body) {
    Compiled compiled(std::string(PRELUDE) + body);
    Config config = no_instruments();
    Context context(compiled.program, config);
    EXPECT_TRUE(context.ok());
    types.context = &context;
    GLTANG_Library * t = nullptr;
    EXPECT_EQ(gltang_library_create("t", &t), GLTANG_OK);
    EXPECT_EQ(gltang_library_add_native(t, "plain", make_with_mode<&Types::plain>, &types), GLTANG_OK);
    EXPECT_EQ(gltang_library_add_native(t, "truthy", make_with_mode<&Types::truthy>, &types), GLTANG_OK);
    EXPECT_EQ(gltang_library_add_native(t, "callee", make_with_mode<&Types::callee>, &types), GLTANG_OK);
    EXPECT_EQ(gltang_library_add_native(t, "texty", make_with_mode<&Types::texty>, &types), GLTANG_OK);
    EXPECT_EQ(gltang_library_add_native(t, "coll", make_coll, &types), GLTANG_OK);
    EXPECT_EQ(gltang_library_add_native(t, "caster", make_with_mode<&Types::caster>, &types), GLTANG_OK);
    EXPECT_EQ(gltang_library_add_native(t, "decliner", make_with_mode<&Types::decliner>, &types), GLTANG_OK);
    EXPECT_EQ(gltang_library_add_native(t, "mark", mark, &types), GLTANG_OK);
    EXPECT_EQ(gltang_library_add_native(t, "marked", marked, &types), GLTANG_OK);
    EXPECT_EQ(gltang_library_add_library(context.library(), t), GLTANG_OK);
    gltang_library_release(t);
    EXPECT_EQ(gltang_execution_attach_types(context.execution, types.registry), GLTANG_OK);
    EXPECT_TRUE(context.execute());
    types.context = nullptr;
    return grcore_context_fuel_used(context.context);
  };
  // A truth test of a type with a `truthy` costs more than of one without: it is a host call.
  uint64_t without = fuel("o = plain(); i = 0; while (i < 100) { if (o) { i += 1; } }");
  uint64_t with = fuel("o = truthy(1); i = 0; while (i < 100) { if (o) { i += 1; } }");
  EXPECT_GT(with, without + 100) << "each truth test charged at least a native call's fuel";
  // A call, likewise, against the same call to a decline.
  uint64_t called = fuel("o = callee(0); i = 0; while (i < 100) { o(1); i += 1; }");
  uint64_t declined = fuel("o = callee(1); i = 0; while (i < 100) { o(1); i += 1; }");
  EXPECT_GT(called, 0u);
  EXPECT_GT(declined, 0u);
}

TEST(TypeOps, ACallableInstanceCannotBePassedWhereOnlyAFunctionIsAccepted) {
  Types types;
  // The engine's test native `reenter` accepts a function and refuses anything else with the same
  // error whatever the value: an instance with a `call` is no better than an integer.
  auto error_of = [&](const char * argument) {
    Out o = run(std::string(PRELUDE) + "use reenter; reenter(" + argument + ", 1);", types, Config(), [](Context & c, Types &) { c.add_native_library(); });
    EXPECT_TRUE(o.finished) << argument;
    return o.kind == GLTANG_KIND_ERROR ? static_cast<int>(o.error) : -1;
  };
  int not_a_function = error_of("5");
  EXPECT_NE(not_a_function, -1);
  EXPECT_EQ(error_of("callee(0)"), not_a_function) << "a callable instance is refused as an integer is";
  EXPECT_EQ(error_of("plain()"), not_a_function);
}

TEST(TypeOps, ACallWithMoreArgumentsThanTheHandleScopeHoldsIsOutOfMemoryAsItIsForANative) {
  Types types;
  std::string args;
  for (int i = 0; i < 1100; ++i) {
    args += (i ? ", " : "") + std::to_string(i);
  }
  Out o = run(PRELUDE "c = callee(0); c(" + args + ");", types);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.kind, GLTANG_KIND_ERROR);
  EXPECT_EQ(o.error, GLTANG_ERROR_OUT_OF_MEMORY) << "the scope's limit (1,024 handles by default) is the one a native's handles have";
  EXPECT_TRUE(o.log.empty()) << "the callback never ran";
}

TEST(TypeOps, EachNewOperationRunsAsAHostFunctionWithOneUnitOfNativeDepth) {
  // A callback asks, from inside, whether it may reconfigure the execution (it may not) and how deep
  // the native budget stands (one unit: its own record).
  struct Probe {
    GLTANG_Execution * execution = nullptr;
    GRCORE_Context * context = nullptr;
    std::vector<uint64_t> depth;
    std::vector<int> attach;
    GLTANG_TypeRegistry * other = nullptr;
    void look() {
      depth.push_back(grcore_context_depth(context, GRCORE_DEPTH_NATIVE));
      // Refused for one reason only: the callback is host code (the other limits are refused once the run has started anyway).
      attach.push_back(static_cast<int>(gltang_execution_set_handle_limits(execution, 0, 0)));
    }
  } probe;
  GLTANG_TypeRegistry * registry = nullptr;
  ASSERT_EQ(gltang_type_registry_create(&registry), GLTANG_OK);
  ASSERT_EQ(gltang_type_registry_create(&probe.other), GLTANG_OK);
  GLTANG_TypeDescriptor d = GLTANG_TYPE_DESCRIPTOR_INIT;
  d.name = "probe";
  d.user = &probe;
  d.thread_safe = true;
  d.iterator_begin = [](GLTANG_NativeCall *, void * u, GLTANG_Handle) { static_cast<Probe *>(u)->look(); return GLTANG_OP_DECLINE; };
  d.iterator_next = [](GLTANG_NativeCall *, void * u, GLTANG_Handle) { static_cast<Probe *>(u)->look(); return GLTANG_OP_DECLINE; };
  d.call = [](GLTANG_NativeCall *, void * u, GLTANG_Handle, const GLTANG_Handle *, size_t) { static_cast<Probe *>(u)->look(); return GLTANG_OP_DECLINE; };
  d.cast = [](GLTANG_NativeCall *, void * u, GLTANG_Handle, GLTANG_CastTarget) { static_cast<Probe *>(u)->look(); return GLTANG_OP_DECLINE; };
  d.truthy = [](GLTANG_NativeCall *, void * u, GLTANG_Handle, bool *) { static_cast<Probe *>(u)->look(); return GLTANG_OP_DECLINE; };
  d.print = [](GLTANG_NativeCall *, void * u, GLTANG_Handle) { static_cast<Probe *>(u)->look(); return GLTANG_OP_DECLINE; };
  d.to_string = [](GLTANG_NativeCall *, void * u, GLTANG_Handle) { static_cast<Probe *>(u)->look(); return GLTANG_OP_DECLINE; };
  const GLTANG_Type * type = nullptr;
  ASSERT_EQ(gltang_type_registry_add(registry, &d, &type), GLTANG_OK);
  ASSERT_EQ(gltang_type_registry_add_attribute(registry, GLTANG_KIND_STRING, "probe", [](GLTANG_NativeCall *, void * u, GLTANG_Handle) { static_cast<Probe *>(u)->look(); return GLTANG_OP_DECLINE; }, &probe), GLTANG_OK);
  struct Maker {
    const GLTANG_Type * type;
  } maker = {type};
  Compiled compiled("use mk; x = mk(); c = x(1); s = x as string; b = x ? 1 : 2; print(x); y = x as int; q = \"s\"; q.probe; for (i : x) { 1; } 7;");
  ASSERT_TRUE(compiled.ok()) << compiled.error.message;
  Context context(compiled.program);
  ASSERT_TRUE(context.ok());
  probe.execution = context.execution;
  probe.context = context.context;
  ASSERT_EQ(gltang_library_add_native(context.library(), "mk", [](GLTANG_NativeCall * call, void * u) {
              GLTANG_Handle h;
              if (gltang_handle_make_custom(call, static_cast<Maker *>(u)->type, &h) != GLTANG_OK) {
                return false;
              }
              gltang_call_return_handle(call, h);
              return true;
            }, &maker), GLTANG_OK);
  ASSERT_EQ(gltang_execution_attach_types(context.execution, registry), GLTANG_OK);
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(context.integer(), 7);
  // call, cast to string (cast, then to_string), truthy, print (print, then to_string), cast to int,
  // the host attribute, and the loop's begin.
  EXPECT_EQ(probe.depth.size(), 9u);
  for (uint64_t depth : probe.depth) {
    EXPECT_EQ(depth, 1u);
  }
  for (int result : probe.attach) {
    EXPECT_EQ(result, static_cast<int>(GLTANG_ERR_INVALID)) << "a host function cannot reconfigure the execution";
  }
  gltang_type_registry_release(registry);
  gltang_type_registry_release(probe.other);
}

TEST(TypeOps, TwoContextsOnTwoThreadsShareOneRegistryOfHostAttributesAndOperations) {
  // One sealed registry, read with no lock by two executions on two threads at once: a built-in
  // attribute that misses (its lookup in the registry), a host attribute that hits, a truth test
  // of an instance. The program and the library are made here, once, and shared (both are
  // immutable once attached): cutil's allocation counters are plain integers, so the threads
  // must not allocate through it, and the callbacks below make nothing with `new`.
  GLTANG_TypeRegistry * registry = nullptr;
  ASSERT_EQ(gltang_type_registry_create(&registry), GLTANG_OK);
  ASSERT_EQ(gltang_type_registry_add_attribute(registry, GLTANG_KIND_STRING, "slug", [](GLTANG_NativeCall * call, void *, GLTANG_Handle self) {
              char buffer[64] = "slug:";
              size_t length = 0;
              const char * text = gltang_handle_text(call, self, &length);
              size_t n = length < 32 ? length : 32;
              if (text) {
                std::memcpy(buffer + 5, text, n);
              }
              GLTANG_Handle h;
              if (gltang_handle_make_string(call, buffer, 5 + n, GLTANG_UNICODE_STRING_TYPE_TRUSTED, &h) != GLTANG_OK) {
                return GLTANG_OP_ERROR;
              }
              gltang_call_return_handle(call, h);
              return GLTANG_OP_OK;
            }, nullptr), GLTANG_OK);
  GLTANG_TypeDescriptor d = GLTANG_TYPE_DESCRIPTOR_INIT;
  d.name = "tt";
  d.thread_safe = true;
  d.truthy = [](GLTANG_NativeCall *, void *, GLTANG_Handle, bool * out) { *out = false; return GLTANG_OP_OK; };
  const GLTANG_Type * type = nullptr;
  ASSERT_EQ(gltang_type_registry_add(registry, &d, &type), GLTANG_OK);
  Compiled compiled("use mk; x = mk(); s = \"ab\"; n = 0; i = 0; while (i < 20) { if (s.slug == \"slug:ab\") { n += 1; } if (!x) { n += 1; } i += 1; } n;");
  ASSERT_TRUE(compiled.ok());
  GLTANG_Library * library = nullptr;
  ASSERT_EQ(gltang_library_create(nullptr, &library), GLTANG_OK);
  struct Maker {
    const GLTANG_Type * type;
  } maker = {type};
  ASSERT_EQ(gltang_library_add_native(library, "mk", [](GLTANG_NativeCall * call, void * u) {
              GLTANG_Handle h;
              if (gltang_handle_make_custom(call, static_cast<Maker *>(u)->type, &h) != GLTANG_OK) {
                return false;
              }
              gltang_call_return_handle(call, h);
              return true;
            }, &maker), GLTANG_OK);
  std::atomic<int> agreed{0};
  const int rounds = tt::heavy_instruments() ? 2 : 10;
  auto body = [&]() {
    for (int round = 0; round < rounds; ++round) {
      Context context(compiled.program);
      EXPECT_EQ(gltang_execution_set_libraries(context.execution, library), GLTANG_OK);
      EXPECT_EQ(gltang_execution_attach_types(context.execution, registry), GLTANG_OK);
      if (context.execute() && context.is_integer() && context.integer() == 40) {
        agreed.fetch_add(1);
      }
    }
  };
  std::thread one(body);
  std::thread two(body);
  one.join();
  two.join();
  gltang_library_release(library);
  gltang_type_registry_release(registry);
  EXPECT_EQ(agreed.load(), 2 * rounds);
}

// ---------------------------------------------------------------------------
// The review's rules (a truthy's error, the kind of a cast's answer, a marked instance,
// polling inside a cast, refused records, injected allocation failures, the names)
// ---------------------------------------------------------------------------

namespace {

/// `run`, with the cutil allocations the whole run left outstanding (blocks made less blocks freed).
Out run_counted(const std::string & source, Types & types, const Config & config, const Setup & setup, long * outstanding) {
  const long before = static_cast<long>(gcu_memory_alloc_count) - static_cast<long>(gcu_memory_free_count);
  Out out = run(source, types, config, setup);
  *outstanding = static_cast<long>(gcu_memory_alloc_count) - static_cast<long>(gcu_memory_free_count) - before;
  return out;
}

}  // namespace

TEST(TypeOps, ATruthyErrorIsTheValueOfNotAndAndAsBoolAndRecordedAsDiscardedWhereItIsTheValueOfNothing) {
  Types types;
  struct Site {
    const char * source;
    bool discarded;
  } sites[] = {
    {"x = !e; [x];", false},
    {"x = e && 1; [x];", false},
    {"x = e as bool; [x];", false},
    {"if (e) { 1; } 7;", true},
    {"i = 0; while (e) { i += 1; if (i > 3) { break; } } 7;", true},
    {"i = 0; do { i += 1; if (i > 3) { break; } } while (e); 7;", true},
    {"x = e ? 1 : 2; [x];", true},
    {"x = e || 5; [x];", true},
  };
  for (const Site & site : sites) {
    Out o = run(std::string(PRELUDE "e = truthy(3); ") + site.source, types);
    ASSERT_TRUE(o.finished) << site.source;
    if (site.discarded) {
      EXPECT_EQ(o.errors_listed, 1u) << site.source << ": the error is recorded as discarded";
      if (o.errors_listed == 1u) {
        EXPECT_EQ(o.hows[0], GLTANG_ERROR_HOW_DISCARDED) << site.source;
        EXPECT_EQ(o.error_kinds[0], GLTANG_ERROR_DIVIDE_BY_ZERO) << site.source;
      }
    }
    else {
      EXPECT_EQ(o.errors_listed, 0u) << site.source << ": the error is a value, held by x";
      EXPECT_NE(o.described.find("Divide by zero"), std::string::npos) << site.source << " gave " << o.described;
    }
  }
}

TEST(TypeOps, ACastAnswerOfTheWrongKindForItsTargetIsHostFunctionFailed) {
  Types types;
  for (const char * target : {"int", "float", "bool", "string"}) {
    Out o = run(std::string(PRELUDE "c = caster(4); c as ") + target + ";", types);
    ASSERT_TRUE(o.finished) << target;
    EXPECT_EQ(o.kind, GLTANG_KIND_ERROR) << target << " gave " << o.described;
    EXPECT_EQ(o.error, GLTANG_ERROR_HOST_FAILED) << target;
  }
  // The right kinds, and an error, still pass.
  Out o = run(PRELUDE "[caster(0) as int, caster(0) as float, caster(0) as bool, caster(0) as string];", types);
  EXPECT_EQ(o.described, "[41, 2.5, false, castS]");
  o = run(PRELUDE "caster(3) as bool;", types);
  EXPECT_EQ(o.error, GLTANG_ERROR_NOT_A_NUMBER);
}

TEST(TypeOps, AMarkedInstanceCastToBoolIsFalseWithoutAskingTheTypeAndOtherCastsAskIt) {
  Types types;
  Out o = run(PRELUDE "k = caster(0); mark(k); [k as bool, k as int, k as string, k ? 1 : 2];", types);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.described, "[false, 41, castS, 2]");
  EXPECT_EQ(o.log, (Log{"caster cast 0", "caster cast 3"})) << "int and string asked, bool and the truth test not";
}

TEST(TypeOps, AMarkedInstanceIsAskedAsTheSourceOfAForTheCalleeOfACall) {
  Types types;
  Out o = run(PRELUDE "e = coll(3); mark(e); n = 0; for (x : e) { n += x; } n;", types);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.integer, 6) << "its iterator_begin and iterator_next ran";
  o = run(PRELUDE "c = callee(0); mark(c); c(5);", types);
  EXPECT_EQ(o.integer, 1005) << "its call ran";
}

TEST(TypeOps, ATruthyThatSetsAnErrorThroughTheCallObjectWithStatusOkIsTheError) {
  Types types;
  for (const char * source : {"x = !t; x;", "x = t && 1; x;", "x = t as bool; x;"}) {
    Out o = run(std::string(PRELUDE "t = truthy(4); ") + source, types);
    ASSERT_TRUE(o.finished) << source;
    EXPECT_EQ(o.kind, GLTANG_KIND_ERROR) << source << " gave " << o.described;
    EXPECT_EQ(o.error, GLTANG_ERROR_NOT_A_NUMBER) << source << ": not overwritten by the boolean";
  }
}

TEST(TypeOps, ANextThatSetsAnErrorThroughTheCallObjectWithStatusOkEndsTheLoopAndIsNotAnItem) {
  Types types;
  Out o = run(PRELUDE "t = []; for (x : coll(5, 10)) { t += [x]; } t;", types);
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.described, "[1]") << "the item before the error; the answer set with the error is not one";
  o = run(PRELUDE "for (x : coll(5, 10)) { 1; }", types);
  EXPECT_EQ(o.kind, GLTANG_KIND_ERROR);
  EXPECT_EQ(o.error, GLTANG_ERROR_NOT_A_NUMBER) << "the loop's value";
}

TEST(TypeOps, ACastCallbackThatPollsIsStoppedByTheBudgetWithTheSamePositionInBothTiers) {
  Types types;
  for (const char * target : {"int", "float", "bool", "string"}) {
    const std::string source = std::string(PRELUDE "p0 = pollcast(0); p1 = pollcast(1); function f(p) { return p as ") + target +
        "; } i = 0; while (i < 20) { f(p0); i += 1; } f(p1); 7;";
    Out reference;
    bool first = true;
    for (long threshold : {0L, 1L}) {
      Config config;
      config.fuel = 150000;
      config.jit_threshold = threshold;
      Out o = run(source, types, config);
      EXPECT_FALSE(o.finished) << target << ": the budget ran out inside the cast (threshold " << threshold << ")";
      EXPECT_NE(o.kind, GLTANG_KIND_INTEGER) << target;
      if (first) {
        reference = o;
        first = false;
        EXPECT_NE(reference.position, 0u);
        EXPECT_EQ(reference.log.size(), 1u) << target << ": the callback saw the identity of its first poll";
        continue;
      }
      EXPECT_COMPILED(o, target);
      EXPECT_EQ(o.position, reference.position) << target << ": the position recorded where the run stopped";
      EXPECT_EQ(o.log, reference.log) << target << ": the identity of the poll in the cast, as each tier writes it";
      EXPECT_EQ(o.fuel, reference.fuel) << target;
    }
    // Without the budget the same program finishes, and the casts answer.
    Out whole = run(std::string(PRELUDE "p = pollcast(0); function f(p) { return p as ") + target + "; } [f(p)];", types);
    EXPECT_TRUE(whole.finished) << target;
  }
}

TEST(TypeOps, ARefusedRecordIsTheRecursionLimitErrorInBothTiersAndLeaksAndLeavesNothing) {
  Types types;
  struct Case {
    const char * name;
    std::string source;
    const char * answer;  // what the program gives when the budget has room
  };
  std::string twenty;
  for (int i = 1; i <= 20; ++i) {
    twenty += (i > 1 ? ", " : "") + std::to_string(i);
  }
  const std::vector<Case> cases = {
    {"call (a heap copy of 21 operands)", "use reenter; function f(c) { return c(" + twenty + "); } reenter(f, callee(0));", "20001"},
    {"cast", "use reenter; function f(c) { return c as int; } reenter(f, caster(0));", "41"},
    {"to_string", "use reenter; function f(c) { return c as string; } reenter(f, texty(2));", "T<&>"},
    {"for", "use reenter; function f(c) { t = []; for (x : c) { t += [x]; } return t; } reenter(f, coll(3));", "[1, 2, 3]"},
  };
  for (const Case & c : cases) {
    for (uint64_t depth = 0; depth <= 4; ++depth) {
      Out reference;
      long reference_outstanding = 0;
      bool first = true;
      for (long threshold : {0L, 1L}) {
        Config config;
        config.native_depth = depth;
        config.jit_threshold = threshold;
        long outstanding = 0;
        Out o = run_counted(std::string(PRELUDE) + c.source, types, config, [](Context & ctx, Types &) {
          ctx.add_native_library();
          ASSERT_EQ(gltang_execution_set_log_all_errors(ctx.execution, true), GLTANG_OK);
        }, &outstanding);
        const std::string where = std::string(c.name) + " at native depth " + std::to_string(depth) + ", threshold " + std::to_string(threshold);
        ASSERT_TRUE(o.finished) << where;
        EXPECT_EQ(outstanding, 0) << where << ": cutil blocks outstanding after the run";
        EXPECT_EQ(o.native_depth, 0u) << where << ": the native depth is back where it started";
        bool limited = false;
        for (GLTANG_ErrorKind k : o.error_kinds) {
          limited = limited || k == GLTANG_ERROR_RECURSION_LIMIT;
        }
        // reenter's nested run takes a unit of its own, so the operation inside needs a depth of 3.
        if (depth <= 2) {
          EXPECT_TRUE(limited) << where << ": a refused record is the recursion-limit error";
          if (std::string(c.name) != "for") {
            EXPECT_EQ(o.kind, GLTANG_KIND_ERROR) << where << " gave " << o.described;
            EXPECT_EQ(o.error, GLTANG_ERROR_RECURSION_LIMIT) << where << ": the value, not Host function failed";
          }
          else if (depth == 2) {
            EXPECT_EQ(o.described, "[]") << where << ": the loop did not run, and no error was an item";
          }
        }
        else {
          EXPECT_FALSE(limited) << where << ": the budget has room";
          EXPECT_EQ(o.described, c.answer) << where;
        }
        if (first) {
          reference = o;
          reference_outstanding = outstanding;
          first = false;
          continue;
        }
        EXPECT_COMPILED(o, where);
        EXPECT_EQ(o.described, reference.described) << where << ": the tiers agree";
        EXPECT_EQ(o.error_kinds, reference.error_kinds) << where;
        EXPECT_EQ(outstanding, reference_outstanding) << where;
      }
    }
  }
}

TEST(TypeOps, AnAllocationThatFailsInsideAnOperationIsOutOfMemoryAndTheRunGoesOnWithNothingPartial) {
  Types types;
  struct Program {
    const char * name;
    std::string source;
    std::function<bool(const Out &)> acceptable;  // an outcome that an allocation failure may leave
    std::function<bool(const Out &)> failed;      // the failure was delivered to the operation under test
  };
  auto oom = [](const Out & o) { return o.finished && o.kind == GLTANG_KIND_ERROR && o.error == GLTANG_ERROR_OUT_OF_MEMORY; };
  // Every whole print, in order, or none of it: a subsequence of the three prints.
  auto whole_prints = [](const std::string & raw) {
    static const char * const all[] = {"a", "<b>&</b>", "b"};
    size_t at = 0;
    for (const char * piece : all) {
      if (raw.compare(at, std::strlen(piece), piece) == 0) {
        at += std::strlen(piece);
      }
    }
    return at == raw.size();
  };
  std::vector<Program> programs = {
    {"print", PRELUDE "print(\"a\"); r = print(texty(0)); print(\"b\"); [r];",
     [&](const Out & o) { return o.finished && whole_prints(o.raw) && (o.described == "[null]" || o.described == "[Error: Out of memory]" || oom(o)); },
     // The print that failed appended nothing, and the print after it still ran. Its value reads as the
     // out-of-memory error or, where the injection came before the run had made its error root, null.
     [&](const Out & o) { return o.raw == "ab"; }},
    {"to_string", PRELUDE "r = texty(2) as string; r;",
     [&](const Out & o) { return o.finished && (o.described == "T<&>" || oom(o)); },
     [&](const Out & o) { return oom(o); }},
    {"cast to string", PRELUDE "c = caster(0); r = c as string; r;",
     [&](const Out & o) { return o.finished && (o.described == "castS" || oom(o)); },
     [&](const Out & o) { return oom(o); }},
    {"cast to float", PRELUDE "c = caster(0); r = c as float; r;",
     [&](const Out & o) { return o.finished && (o.described == "2.5" || oom(o)); },
     [&](const Out & o) { return oom(o); }},
    {"iterator step", PRELUDE "t = []; for (x : coll(3, 1)) { t += [x]; } t;",
     [&](const Out & o) {
       return o.finished && (o.described == "[]" || o.described == "[item1]" || o.described == "[item1, item2]" ||
                             o.described == "[item1, item2, item3]" || oom(o));
     },
     [&](const Out & o) { return o.described == "[]" || o.described == "[item1]" || o.described == "[item1, item2]"; }},
  };
  for (const Program & program : programs) {
    int delivered = 0;
    int in_operation = 0;
    for (long n = 1; n < 2000; ++n) {
      Config config;
      config.fail_at = n;
      config.torture = 0;
      config.verify = 0;
      config.moving_stack = 0;
      config.relocate = 0;
      config.jit_threshold = 0;
      g_injecting = true;
      Out o = run(program.source, types, config);
      g_injecting = false;
      if (!o.fired) {
        if (o.finished) {
          break;  // past the last allocation of the run
        }
        continue;
      }
      ++delivered;
      if ((!o.finished && o.kind == GLTANG_KIND_NULL && o.described.empty()) || (o.kind == GLTANG_KIND_ERROR && o.error == GLTANG_ERROR_NOT_IMPLEMENTED)) {
        continue;  // the context could not be made, or the library of the setup could not (`use t` is Not implemented)
      }
      EXPECT_TRUE(program.acceptable(o)) << program.name << " with allocation " << n << " failed gave " << o.described << " / raw '" << o.raw << "'";
      in_operation += program.failed(o) ? 1 : 0;
    }
    EXPECT_GT(delivered, 5) << program.name << ": the sweep delivered failures";
    EXPECT_GT(in_operation, 0) << program.name << ": at least one failure landed inside the operation, with the exact error";
  }
}

TEST(TypeOps, EveryBuiltInMemberNameWinsOverAHostAttributeOfTheSameName) {
  // Every member name of the string, array and generator kinds is registered as a host attribute
  // answering 999 and counting; none is reached for its own kind, and a name that is not a member is.
  static std::atomic<int> hits;
  hits = 0;
  GLTANG_TypeRegistry * registry = nullptr;
  ASSERT_EQ(gltang_type_registry_create(&registry), GLTANG_OK);
  auto answer = [](GLTANG_NativeCall * call, void *, GLTANG_Handle) {
    hits.fetch_add(1);
    gltang_call_return_integer(call, 999);
    return GLTANG_OP_OK;
  };
  for (const char * name : {"length", "byte_length", "html", "html_attribute", "percent", "javascript", "raw", "render", "shout"}) {
    ASSERT_EQ(gltang_type_registry_add_attribute(registry, GLTANG_KIND_STRING, name, answer, nullptr), GLTANG_OK) << name;
  }
  for (const char * name : {"size", "extra"}) {
    ASSERT_EQ(gltang_type_registry_add_attribute(registry, GLTANG_KIND_ARRAY, name, answer, nullptr), GLTANG_OK) << name;
  }
  for (const char * name : {"next_int", "next_float", "next_bool", "set_seed", "next_gaussian"}) {
    ASSERT_EQ(gltang_type_registry_add_attribute(registry, GLTANG_KIND_RNG, name, answer, nullptr), GLTANG_OK) << name;
  }
  Compiled compiled(
      "use random; s = \"a\"; a = [1]; r = random.seeded(1); "
      "m = [s.length, s.byte_length, s.html, s.html_attribute, s.percent, s.javascript, s.raw, s.render, a.size, r.next_int, r.next_float, r.next_bool, r.set_seed]; "
      "[m.size, s.shout, a.extra, r.next_gaussian];");
  ASSERT_TRUE(compiled.ok());
  Context context(compiled.program);
  ASSERT_TRUE(context.ok());
  ASSERT_EQ(gltang_execution_attach_types(context.execution, registry), GLTANG_OK);
  ASSERT_TRUE(context.execute());
  EXPECT_EQ(context.describe(), "[13, 999, 999, 999]");
  EXPECT_EQ(hits.load(), 3) << "the host attributes of names that are not members, and only those";
  gltang_type_registry_release(registry);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
