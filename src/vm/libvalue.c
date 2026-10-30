/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Lang-tang.
 *
 * Ghoti.io Lang-tang is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License version
 * 3 as published by the Free Software Foundation.
 *
 * Ghoti.io Lang-tang is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * Libraries, native functions and generators as values: the heap objects, the
 * resolution of a `use` (language reference, section 9.1), the attribute rule
 * on a library and on a generator, and the call of a native function.
 *
 * Host functions are opaque (AD-23). They run synchronously, on the context's
 * owner thread, inside the run, and are given a call object and their `user`
 * pointer only. `in_host` is set around them so that any `gltang_execution_*`
 * mutator they reach is refused; `grcore_run` and `grcore_resume` are refused
 * by runtime-core, which sees a context that is running.
 */

#include "test_hooks.h"
#include <ghoti.io/lang-tang/macros.h>

#include <string.h>
#include <ghoti.io/cutil/memory.h>
#include <ghoti.io/runtime-core/a/activation.h>
#include <ghoti.io/runtime-heap/heap.h>
#include "description_internal.h"
#include "vm_internal.h"
#ifdef GLTANG_WITH_JIT
#include "../jit/jit.h"
#endif

// ---------------------------------------------------------------------------
// The heap types
// ---------------------------------------------------------------------------

static void native_trace(GRHEAP_Tracer * tracer, void * payload) {
  GLTANG_NativeObject * native = payload;
  grheap_trace_word(tracer, &native->bound);
}

// A library, a native function and a template hold a pointer to a host object:
// the hooks write its name and resolve it again (snapshot.c).
const GRHEAP_Type gltang_type_library = GRHEAP_TYPE_INIT("lang-tang library", sizeof(GLTANG_LibraryObject), NULL, NULL, NULL, gltang_vm_library_snapshot, gltang_vm_library_restore);
const GRHEAP_Type gltang_type_native = GRHEAP_TYPE_INIT("lang-tang native function", sizeof(GLTANG_NativeObject), native_trace, NULL, NULL, gltang_vm_native_snapshot, gltang_vm_native_restore);
const GRHEAP_Type gltang_type_template = GRHEAP_TYPE_INIT("lang-tang template", sizeof(GLTANG_TemplateObject), NULL, NULL, NULL, gltang_vm_template_snapshot, gltang_vm_template_restore);
const GRHEAP_Type gltang_type_rng = GRHEAP_TYPE_INIT("lang-tang generator", sizeof(GLTANG_RngObject), NULL, NULL, NULL, NULL, NULL);

static GLTANG_Value library_value(GLTANG_Execution * exec, const GLTANG_Library * library) {
  void * object;
  GLTANG_Status st = gltang_vm_alloc(exec, &gltang_type_library, sizeof(GLTANG_LibraryObject), &object);
  if (st != GLTANG_ST_OK) {
    return gltang_vm_failure_value(exec, st);
  }
  GLTANG_LibraryObject * value = object;
  value->kind = GLTANG_OBJ_LIBRARY;
  value->library = library;
  return gltang_value_of(value);
}

GLTANG_Value gltang_vm_make_library_value(GLTANG_Execution * exec, const GLTANG_Library * library) {
  return library_value(exec, library);
}

static GLTANG_Value native_value(GLTANG_Execution * exec, const GLTANG_LibraryMember * member, GLTANG_BuiltinId builtin, GLTANG_Value bound) {
  // The value it is bound to is a parameter, held by nothing the collector
  // reads: hold it across the allocation and take it from there afterwards.
  size_t mark = gltang_vm_temp_mark(exec);
  if (!gltang_vm_temp_push(exec, bound)) {
    return exec->roots[GLTANG_ROOT_OOM];
  }
  void * object;
  GLTANG_Status st = gltang_vm_alloc(exec, &gltang_type_native, sizeof(GLTANG_NativeObject), &object);
  bound = gltang_vm_temp_at(exec, mark);
  gltang_vm_temp_release(exec, mark);
  if (st != GLTANG_ST_OK) {
    return gltang_vm_failure_value(exec, st);
  }
  GLTANG_NativeObject * value = object;
  value->kind = GLTANG_OBJ_NATIVE;
  value->builtin = (uint32_t)builtin | (member && (member->native_flags & GLTANG_NATIVE_FLAG_RESUMABLE) ? GLTANG_NATIVE_RESUMABLE_BIT : 0u);
  value->member = member;
  (void)grheap_store_word(exec->heap, value, &value->bound, bound);
  return gltang_value_of(value);
}

GLTANG_Value gltang_vm_make_native_value(GLTANG_Execution * exec, const GLTANG_LibraryMember * member, GLTANG_Value bound) {
  return native_value(exec, member, GLTANG_BUILTIN_NONE, bound);
}

static GLTANG_Value template_value(GLTANG_Execution * exec, const GLTANG_LibraryMember * member) {
  void * object;
  GLTANG_Status st = gltang_vm_alloc(exec, &gltang_type_template, sizeof(GLTANG_TemplateObject), &object);
  if (st != GLTANG_ST_OK) {
    return gltang_vm_failure_value(exec, st);
  }
  GLTANG_TemplateObject * value = object;
  value->kind = GLTANG_OBJ_TEMPLATE;
  value->member = member;
  return gltang_value_of(value);
}

// ---------------------------------------------------------------------------
// Generators (CAP-6, AD-25)
// ---------------------------------------------------------------------------

/** The next seed of the execution's sequence; makes a private one from entropy first if there is none. */
static bool next_seed(GLTANG_Execution * exec, uint64_t * seed) {
  if (!exec->seeds) {
    GLTANG_SeedSequence * seeds = NULL;
    if (gltang_seeds_create_random(&seeds) != GLTANG_OK) {
      return false;
    }
    exec->seeds = seeds;
  }
  *seed = gltang_seeds_next(exec->seeds);
  return true;
}

static GLTANG_Value rng_new(GLTANG_Execution * exec, uint64_t seed, bool is_global) {
  void * object;
  GLTANG_Status st = gltang_vm_alloc(exec, &gltang_type_rng, sizeof(GLTANG_RngObject), &object);
  if (st != GLTANG_ST_OK) {
    return gltang_vm_failure_value(exec, st);
  }
  GLTANG_RngObject * rng = object;
  rng->kind = GLTANG_OBJ_RNG;
  rng->is_global = is_global ? 1u : 0u;
  rng->seed = seed;
  gcu_random_mt64_init(&rng->state, seed);
  return gltang_value_of(rng);
}

static GLTANG_RngObject * rng_of(GLTANG_Value v) {
  return gltang_object(v);
}

/** The generator's own members: the one list, read by the attribute rule and by the test that asks whether a name is one. */
typedef enum { RNG_NEXT_INT, RNG_NEXT_FLOAT, RNG_NEXT_BOOL, RNG_SET_SEED, RNG_NONE } RngMember;

static RngMember rng_member(const char * name, size_t length) {
  static const struct {
    const char * name;
    size_t length;
  } members[RNG_NONE] = {
    [RNG_NEXT_INT] = {"next_int", 8},
    [RNG_NEXT_FLOAT] = {"next_float", 10},
    [RNG_NEXT_BOOL] = {"next_bool", 9},
    [RNG_SET_SEED] = {"set_seed", 8},
  };
  for (int m = 0; m < RNG_NONE; ++m) {
    if (members[m].length == length && !memcmp(name, members[m].name, length)) {
      return (RngMember)m;
    }
  }
  return RNG_NONE;
}

GLTANG_Value gltang_vm_rng_try_attr(GLTANG_Execution * exec, GLTANG_Value rng, const char * name, size_t length, bool * found) {
  GLTANG_RngObject * state = rng_of(rng);
  *found = true;
  switch (rng_member(name, length)) {
    case RNG_NEXT_INT:
      return gltang_vm_make_int(exec, (int64_t)gcu_random_mt64_next(&state->state));
    case RNG_NEXT_FLOAT: {
      uint64_t word = gcu_random_mt64_next(&state->state);
      return gltang_vm_make_float(exec, (double)(word >> 11) * (1.0 / 9007199254740992.0));
    }
    case RNG_NEXT_BOOL:
      return gltang_v_from_bool((gcu_random_mt64_next(&state->state) & 1u) != 0);
    case RNG_SET_SEED:
      return native_value(exec, NULL, GLTANG_BUILTIN_RNG_SET_SEED, rng);
    case RNG_NONE:
      break;
  }
  *found = false;
  return GLTANG_V_NULL;
}

GLTANG_Value gltang_vm_rng_attr(GLTANG_Execution * exec, GLTANG_Value rng, const char * name, size_t length) {
  bool found;
  GLTANG_Value r = gltang_vm_rng_try_attr(exec, rng, name, length, &found);
  return found ? r : gltang_vm_make_error(exec, GLTANG_ERROR_NOT_IMPLEMENTED);
}

// ---------------------------------------------------------------------------
// Members as values
// ---------------------------------------------------------------------------

static GLTANG_Value host_value(GLTANG_Execution * exec, const GLTANG_HostValue * host) {
  switch (host->kind) {
    case GLTANG_HOST_BOOL: return gltang_v_from_bool(host->boolean);
    case GLTANG_HOST_INTEGER: return gltang_vm_make_int(exec, host->integer);
    case GLTANG_HOST_FLOAT: return gltang_vm_make_float(exec, host->number);
    case GLTANG_HOST_STRING:
      if (!host->text || (unsigned)host->encoding > (unsigned)GLTANG_UNICODE_STRING_TYPE_JAVASCRIPT) {
        return GLTANG_V_NULL;
      }
      return gltang_vm_string_from_utf8(exec, host->text, host->length, host->encoding);
    default:
      return GLTANG_V_NULL;
  }
}

static GLTANG_Value call_result_general(GLTANG_Execution * exec, GLTANG_NativeCall * call, bool worked);
static inline GLTANG_Value call_result(GLTANG_Execution * exec, GLTANG_NativeCall * call, bool worked);
static inline void call_finish(GLTANG_NativeCall * call);

GLTANG_Value gltang_vm_member_value(GLTANG_Execution * exec, const GLTANG_LibraryMember * member) {
  switch (member->kind) {
    case GLTANG_MEMBER_NULL: return GLTANG_V_NULL;
    case GLTANG_MEMBER_BOOL: return gltang_v_from_bool(member->boolean);
    case GLTANG_MEMBER_INTEGER: return gltang_vm_make_int(exec, member->integer);
    case GLTANG_MEMBER_FLOAT: return gltang_vm_make_float(exec, member->number);
    case GLTANG_MEMBER_STRING:
      return gltang_vm_string_from_utf8(exec, member->text, member->length, member->encoding);
    case GLTANG_MEMBER_NATIVE: return native_value(exec, member, GLTANG_BUILTIN_NONE, GLTANG_V_NULL);
    case GLTANG_MEMBER_TEMPLATE: return template_value(exec, member);
    case GLTANG_MEMBER_LIBRARY: return library_value(exec, member->library);
    case GLTANG_MEMBER_FACTORY: {
      // A factory is called each time a `use` that reaches it executes, and
      // never at registration. It makes a scalar; "not available" is null.
      GLTANG_HostValue host;
      memset(&host, 0, sizeof(host));
      const bool outer_in_host = exec->in_host;
      exec->in_host = true;
      bool available = member->factory(member->user, &host);
      exec->in_host = outer_in_host;
      return available ? host_value(exec, &host) : GLTANG_V_NULL;
    }
    case GLTANG_MEMBER_HANDLE_FACTORY: {
      // A factory that answers with any value: it is given a call object with no
      // arguments, makes what it answers with handles, and is read as a native is.
      GLTANG_NativeCall call;
      memset(&call, 0, sizeof(call));
      call.exec = exec;
      const bool outer_in_host = exec->in_host;
      exec->in_host = true;
      bool available = member->native(&call, member->user);
      exec->in_host = outer_in_host;
      if (!available) {
        gltang_vm_scope_close(&call);
        call_finish(&call);
        return GLTANG_V_NULL;
      }
      return call_result(exec, &call, true);
    }
    case GLTANG_MEMBER_VALUE:
      // A fresh copy for each reach: never cached, never shared with another
      // reach or another execution.
      return gltang_vm_materialize(exec, member->description);
    case GLTANG_MEMBER_VALUE_FACTORY: {
      // Called each time a `use` that reaches it executes, and never at
      // registration. It fills a description that is the context's own (its
      // memory is charged to the context) and that is gone when the factory
      // returns; the value is made from it after the factory has returned and
      // the host-function flag is back as it was.
      GLTANG_ValueDescription * description;
      if (gltang_vm_description_create_in_run(exec->allocator, &description) != GLTANG_OK) {
        return GLTANG_NATIVE_POLL(exec, ALLOCATION_REFUSED, 0) != GLTANG_ST_OK ? GLTANG_V_UNWIND : exec->roots[GLTANG_ROOT_OOM];
      }
      description->host_context = exec->host_context;
      const bool outer_in_host = exec->in_host;
      exec->in_host = true;
      bool available = member->value_factory(member->user, description);
      exec->in_host = outer_in_host;
      GLTANG_Value result = available ? gltang_vm_materialize(exec, description) : GLTANG_V_NULL;
      gltang_vm_description_destroy_in_run(description);
      return result;
    }
    case GLTANG_MEMBER_BUILTIN:
      switch (member->builtin) {
        case GLTANG_BUILTIN_RANDOM_GLOBAL: {
          // One per execution, made on first access.
          if (!exec->roots[GLTANG_ROOT_RANDOM]) {
            uint64_t seed;
            if (!next_seed(exec, &seed)) {
              return gltang_vm_make_error(exec, GLTANG_ERROR_HOST_FAILED);
            }
            GLTANG_Value rng = rng_new(exec, seed, true);
            if (rng == GLTANG_V_UNWIND || rng == exec->roots[GLTANG_ROOT_OOM]) {
              return rng;
            }
            exec->roots[GLTANG_ROOT_RANDOM] = rng;
          }
          return exec->roots[GLTANG_ROOT_RANDOM];
        }
        case GLTANG_BUILTIN_RANDOM_DEFAULT: {
          uint64_t seed;
          if (!next_seed(exec, &seed)) {
            return gltang_vm_make_error(exec, GLTANG_ERROR_HOST_FAILED);
          }
          return rng_new(exec, seed, false);
        }
        case GLTANG_BUILTIN_RANDOM_SEEDED:
          return native_value(exec, member, GLTANG_BUILTIN_RANDOM_SEEDED, GLTANG_V_NULL);
        default:
          return GLTANG_V_NULL;
      }
  }
  return GLTANG_V_NULL;
}

GLTANG_Value gltang_vm_library_attr(GLTANG_Execution * exec, GLTANG_Value library, const char * name, size_t length) {
  const GLTANG_Library * lib = ((const GLTANG_LibraryObject *)gltang_object(library))->library;
  const GLTANG_LibraryMember * member = gltang_library_find(lib, name, length);
  if (!member) {
    return gltang_vm_make_error(exec, GLTANG_ERROR_NOT_IMPLEMENTED);
  }
  return gltang_vm_member_value(exec, member);
}

// ---------------------------------------------------------------------------
// Resolving a use
// ---------------------------------------------------------------------------

GLTANG_Value gltang_vm_resolve(GLTANG_Execution * exec, const GLTANG_StringBlock * path) {
  const char * text = gltang_string_bytes(path);
  size_t length = (size_t)path->byte_length;
  const char * dot = memchr(text, '.', length);
  size_t first = dot ? (size_t)(dot - text) : length;
  // The first name is looked up in the execution's libraries, then the
  // program's, then the language layer, then the built-ins; the first that has
  // it wins.
  const GLTANG_Library * layers[4] = {
    exec->libraries,
    gltang_exec_current_program(exec)->libraries,
    exec->language,
    gltang_library_builtins(),
  };
  const GLTANG_LibraryMember * member = NULL;
  for (size_t i = 0; i < 4 && !member; ++i) {
    // Most layers are empty (no execution library, no language layer): not a call to say so.
    if (layers[i]) {
      member = gltang_library_find(layers[i], text, first);
    }
  }
  if (!member) {
    return GLTANG_V_NULL;
  }
  GLTANG_Value v = gltang_vm_member_value(exec, member);
  size_t at = first;
  while (at < length && v != GLTANG_V_UNWIND) {
    // The rest of the path is read with the engine's attribute rule.
    ++at;  // the dot
    size_t next = at;
    while (next < length && text[next] != '.') {
      ++next;
    }
    if (!gltang_vm_temp_push(exec, v)) {
      return exec->roots[GLTANG_ROOT_OOM];
    }
    v = gltang_vm_op_attr_named(exec, v, text + at, next - at);
    gltang_vm_temp_pop(exec);
    at = next;
  }
  return v;
}

// ---------------------------------------------------------------------------
// Calling a native function
// ---------------------------------------------------------------------------

size_t gltang_call_count(const GLTANG_NativeCall * call) {
  return call ? call->argc : 0;
}

GLTANG_ValueKind gltang_call_kind(const GLTANG_NativeCall * call, size_t index) {
  return call && index < call->argc ? gltang_vm_kind(call->args[index]) : GLTANG_KIND_NULL;
}

bool gltang_call_bool(const GLTANG_NativeCall * call, size_t index) {
  return call && index < call->argc && call->args[index] == GLTANG_V_TRUE;
}

int64_t gltang_call_integer(const GLTANG_NativeCall * call, size_t index) {
  return gltang_call_kind(call, index) == GLTANG_KIND_INTEGER ? gltang_vm_int(call->args[index]) : 0;
}

double gltang_call_float(const GLTANG_NativeCall * call, size_t index) {
  return gltang_call_kind(call, index) == GLTANG_KIND_FLOAT ? gltang_vm_float(call->args[index]) : 0.0;
}

const char * gltang_call_text(const GLTANG_NativeCall * call, size_t index, size_t * out_length) {
  if (gltang_call_kind(call, index) != GLTANG_KIND_STRING) {
    return NULL;
  }
  const GLTANG_StringBlock * s = gltang_vm_string(call->args[index]);
  if (out_length) {
    *out_length = (size_t)s->byte_length;
  }
  return gltang_string_bytes(s);
}

/** Forgets the answer so far (the next `gltang_call_return_*` replaces it). */
static void call_clear_slow(GLTANG_NativeCall * call) {
  gcu_free(call->text);
  call->text = NULL;
  // A description kept by the host was retained for the call; the call's own is
  // freed when the call ends, whatever was answered.
  if (call->answer && call->answer != call->built) {
    gltang_value_description_release(call->answer);
  }
  call->answer = NULL;
}

// Most answers are a scalar and most calls build nothing: the common case is
// two loads and a branch, and the work is out of line.
static inline void call_clear(GLTANG_NativeCall * call) {
  if (call->text || call->answer) {
    call_clear_slow(call);
  }
}

void gltang_vm_call_forget_answer(GLTANG_NativeCall * call) {
  call_clear(call);
}

/** The end of a call: the answer is dropped and so is the description the native built. */
static inline void call_finish(GLTANG_NativeCall * call) {
  call_clear(call);
  if (call->built) {
    gltang_vm_description_destroy_in_run(call->built);
    call->built = NULL;
  }
}

void gltang_call_return_null(GLTANG_NativeCall * call) {
  if (call) {
    call_clear(call);
    call->set = GLTANG_CALL_NULL;
  }
}

void gltang_call_return_bool(GLTANG_NativeCall * call, bool value) {
  if (call) {
    call_clear(call);
    call->set = GLTANG_CALL_BOOL;
    call->boolean = value;
  }
}

void gltang_call_return_integer(GLTANG_NativeCall * call, int64_t value) {
  if (call) {
    call_clear(call);
    call->set = GLTANG_CALL_INTEGER;
    call->integer = value;
  }
}

void gltang_call_return_float(GLTANG_NativeCall * call, double value) {
  if (call) {
    call_clear(call);
    call->set = GLTANG_CALL_FLOAT;
    call->number = value;
  }
}

void gltang_call_return_string(GLTANG_NativeCall * call, const char * text, size_t length, GLTANG_String_Type encoding) {
  if (!call) {
    return;
  }
  call_clear(call);
  call->set = GLTANG_CALL_STRING;
  call->encoding = encoding;
  call->length = length;
  call->failed_to_copy = (unsigned)encoding > (unsigned)GLTANG_UNICODE_STRING_TYPE_JAVASCRIPT || length == SIZE_MAX;
  if (call->failed_to_copy) {
    return;
  }
  // Copied now: the function's own buffer may be gone when it returns.
  call->text = gcu_malloc(length + 1u);
  if (!call->text || (!text && length)) {
    gcu_free(call->text);
    call->text = NULL;
    call->failed_to_copy = true;
    return;
  }
  if (length) {
    memcpy(call->text, text, length);
  }
  call->text[length] = '\0';
}

GLTANG_Result gltang_call_description(GLTANG_NativeCall * call, GLTANG_ValueDescription ** out_description) {
  if (!call || !out_description || call->built) {
    return GLTANG_ERR_INVALID;
  }
  // The context's allocator: what the host builds during a call is charged to
  // the context, and fails when its memory budget does.
  GLTANG_Result r = gltang_vm_description_create_in_run(call->exec->allocator, &call->built);
  if (r != GLTANG_OK) {
    call->built = NULL;
    return r;
  }
  *out_description = call->built;
  return GLTANG_OK;
}

void gltang_call_return_value(GLTANG_NativeCall * call, GLTANG_ValueDescription * description) {
  if (!call) {
    return;
  }
  call_clear(call);
  if (!description) {
    call->set = GLTANG_CALL_NULL;
    return;
  }
  if (description == call->built) {
    call->answer = description;
  }
  else {
    // One the host keeps: sealed by being answered, and held for the call. One
    // that belongs to another call cannot be kept, and is not an answer.
    GLTANG_ValueDescription * held = gltang_value_description_retain(description);
    if (!held) {
      call->set = GLTANG_CALL_ERROR;
      call->error = GLTANG_ERROR_HOST_FAILED;
      return;
    }
    (void)gltang_value_description_seal(held);
    call->answer = held;
  }
  call->set = GLTANG_CALL_DESCRIPTION;
}

void gltang_call_return_error(GLTANG_NativeCall * call, GLTANG_ErrorKind kind) {
  if (call) {
    call_clear(call);
    call->set = GLTANG_CALL_ERROR;
    call->error = kind;
  }
}

/** The value a finished host call stands for. */
static GLTANG_Value call_result_general(GLTANG_Execution * exec, GLTANG_NativeCall * call, bool worked) {
  GLTANG_Value result;
  if (call->scope_unwind) {
    // A constructor's poll ordered the run to stop.
    result = GLTANG_V_UNWIND;
  }
  else if (call->scope_oom) {
    // A handle or a value could not be made: the callback's own out-of-memory result.
    result = exec->roots[GLTANG_ROOT_OOM];
  }
  else if (call->set == GLTANG_CALL_ERROR) {
    GLTANG_ErrorKind kind = (unsigned)call->error < (unsigned)GLTANG_ERROR_KIND_COUNT ? call->error : GLTANG_ERROR_HOST_FAILED;
    result = gltang_vm_make_error(exec, kind);
  }
  else if (!worked || call->failed_to_copy) {
    result = gltang_vm_make_error(exec, GLTANG_ERROR_HOST_FAILED);
  }
  else {
    switch (call->set) {
      case GLTANG_CALL_BOOL: result = gltang_v_from_bool(call->boolean); break;
      case GLTANG_CALL_INTEGER: result = gltang_vm_make_int(exec, call->integer); break;
      case GLTANG_CALL_FLOAT: result = gltang_vm_make_float(exec, call->number); break;
      case GLTANG_CALL_VALUE: result = call->value; break;
      case GLTANG_CALL_HANDLE: {
        // Read now, while the scope that holds it is open; nothing allocates between here and the caller's store.
        bool ok;
        result = gltang_vm_handle_value(call, call->answer_handle, &ok);
        if (!ok) {
          result = gltang_vm_make_error(exec, GLTANG_ERROR_HOST_FAILED);
        }
        break;
      }
      case GLTANG_CALL_DESCRIPTION:
        // A fresh copy in the heap of this execution, paced and charged as it is
        // made. The call's own description is still alive; it is freed with the call.
        result = gltang_vm_materialize(exec, call->answer);
        break;
      case GLTANG_CALL_STRING:
        // A native costs fuel in proportion to the bytes it returns.
        exec->pending_fuel += call->length / GLTANG_WORK_BYTES_PER_FUEL;
        result = gltang_vm_string_from_utf8(exec, call->text, call->length, call->encoding);
        if (result == GLTANG_V_NULL && call->length) {
          result = gltang_vm_make_error(exec, GLTANG_ERROR_HOST_FAILED);
        }
        break;
      default: result = GLTANG_V_NULL; break;
    }
  }
  gltang_vm_scope_close(call);
  call_finish(call);
  return result;
}

GLTANG_Value gltang_vm_call_settle(GLTANG_Execution * exec, GLTANG_NativeCall * call, bool worked) {
  return call_result_general(exec, call, worked);
}

void gltang_vm_call_abandon(GLTANG_NativeCall * call) {
  gltang_vm_scope_close(call);
  call_finish(call);
}

/**
 * The value a finished host call stands for. A scalar answer from a call that
 * used no handle, no description and no text (nearly every native call) is made
 * here, inline; everything else is the general function's.
 */
static inline GLTANG_Value call_result(GLTANG_Execution * exec, GLTANG_NativeCall * call, bool worked) {
  if (worked && !call->scope && !call->scope_oom && !call->scope_unwind && !call->failed_to_copy) {
    GLTANG_Value result;
    switch (call->set) {
      case GLTANG_CALL_NONE:
      case GLTANG_CALL_NULL: result = GLTANG_V_NULL; break;
      case GLTANG_CALL_BOOL: result = gltang_v_from_bool(call->boolean); break;
      case GLTANG_CALL_INTEGER: result = gltang_vm_make_int(exec, call->integer); break;
      case GLTANG_CALL_FLOAT: result = gltang_vm_make_float(exec, call->number); break;
      default: return call_result_general(exec, call, worked);
    }
    call_finish(call);
    return result;
  }
  return call_result_general(exec, call, worked);
}

static bool integer_argument(GLTANG_Value v) {
  return gltang_vm_kind(v) == GLTANG_KIND_INTEGER;
}

/**
 * The value of a native itself, once its activation record is open: the
 * engine's own builtins, then a host function (opaque, AD-23: it cannot reach the
 * heap, pause or call guest code through the library's API). Test natives
 * (src/vm/testnatives.c) are host functions that reach the engine's internals.
 */
/**
 * The words a resumable native's frame keeps after the arguments, covered by the record's
 * conservative segment as the operation brackets' are (custom.c): a continuation's state and
 * result come in through the first two, and a request goes out through the rest.
 */
#define N_STATE 0u
#define N_RESULT 1u
#define N_CALLEE 2u
#define N_ARGS 3u
#define N_NEXT_STATE (N_ARGS + GLTANG_GUEST_CALL_ARGS_MAX)
#define N_EXTRA (N_NEXT_STATE + 1u)

/**
 * The value of a resumable native, once its activation record is open (AD-23): a host function that may ask the
 * interpreter to call guest code, through gltang_call_guest, and to be called again with the result. `extra` is its
 * frame past the arguments. A request made at an instruction (or by the call a pending record waits for) becomes a
 * pending record and the result is GLTANG_V_UNWIND with `request_made` set; made anywhere else it is run nested
 * here. This is the cold twin of run_native, which every other native takes and which knows none of it.
 */
static GLTANG_Value run_resumable_native(GLTANG_Execution * exec, GLTANG_Value callee, size_t argc, const GLTANG_Value * args, GLTANG_Value * extra) {
  const GLTANG_NativeObject * native = gltang_object(callee);
  const GLTANG_LibraryMember * member = native->member;
  bool continuing = false;
  uint32_t continuation_index = 0;
  if (exec->resume_active) {
    // The call is run again to continue from the pending record: this native's, or a bug.
    const GLTANG_PendingObject * record = gltang_object(exec->pending_top);
    if (!extra || record->member != member || record->type) {
      gltang_vm_pending_pop(exec);
      exec->resume_active = false;
      return gltang_vm_make_error(exec, GLTANG_ERROR_HOST_FAILED);
    }
    continuing = true;
    continuation_index = record->continuation;
    extra[N_STATE] = record->slots[GLTANG_PENDING_STATE];
    extra[N_RESULT] = record->slots[GLTANG_PENDING_RESULT];
    gltang_vm_pending_pop(exec);
    exec->resume_active = false;
    // A continuation is a native call: charged as the call was.
    exec->pending_fuel += gltang_opcode_cost_table[GLTANG_OP_CALL];
  }
  for (;;) {
    GLTANG_NativeCall call;
    memset(&call, 0, GLTANG_CALL_ZEROED);
    call.exec = exec;
    call.args = args;
    call.argc = argc;
    // The value the native is bound to (gltang_call_bound).
    call.resume_state = native->bound;
    call.owner = member;
    call.owner_kind = 2;
    // A native that re-entered guest code may have called another native: the outer
    // one is still running when the inner returns, so the flag goes back as it was.
    const bool outer_in_host = exec->in_host;
    exec->in_host = true;
    bool worked;
    GLTANG_OpStatus status = GLTANG_OP_OK;
    if (continuing) {
      GLTANG_Handle h_state;
      GLTANG_Handle h_result;
      const bool pushed = gltang_vm_handle_push(&call, extra[N_STATE], &h_state) == GLTANG_OK && gltang_vm_handle_push(&call, extra[N_RESULT], &h_result) == GLTANG_OK;
      if (pushed) {
        status = member->continuations[continuation_index].function(&call, member->user, h_state, h_result);
      }
      // OK is the value set; ERROR is the error set (`Host function failed` without a kind); DECLINE,
      // DONE and a PENDING that asked for nothing are `Host function failed`.
      worked = pushed && status == GLTANG_OP_OK;
    }
    else {
      worked = member->native(&call, member->user);
    }
    exec->in_host = outer_in_host;
    GLTANG_HandleState * hs = exec->handles;
    const bool requested = hs && hs->request.made;
    const bool wants = continuing ? status == GLTANG_OP_PENDING : worked;
    const bool asked = requested && wants && !call.scope_unwind && !call.scope_oom && call.set != GLTANG_CALL_ERROR;
    if (requested && !asked) {
      // A request followed by anything else is a failure, and runs nothing.
      hs->request.made = false;
      worked = false;
    }
    if (!asked) {
      GLTANG_Value result = call_result(exec, &call, worked);
      // Last, so that a nested call the native made does not leave its own status here.
      exec->native_status = call.test_status;
      return result;
    }
    // A guest call was asked for: copy it out of the handles into words the collector does not
    // move, and close the callback (its scope, handles and text pointers go; its record is left by
    // the caller of this function, before the interpreter starts the callee).
    const GLTANG_GuestRequest * request = &hs->request;
    extra[N_CALLEE] = request->callee;
    for (uint32_t i = 0; i < request->argc; ++i) {
      extra[N_ARGS + i] = request->args[i];
    }
    extra[N_NEXT_STATE] = request->state;
    const uint32_t request_argc = request->argc;
    const uint32_t request_index = request->continuation;
    hs->request.made = false;
    gltang_vm_call_abandon(&call);
    exec->native_status = GLTANG_NATIVE_STATUS_OK;
    if (exec->suspend_mode != 0) {
      const bool chained = exec->suspend_mode == 2u;
      GLTANG_PendingSpec spec = {
        .type = NULL, .member = member, .op = GLTANG_TYPE_OP_COUNT, .step = 0, .continuation = request_index,
        .chained = chained, .state = extra[N_NEXT_STATE], .callee = extra[N_CALLEE],
        .args = &extra[N_ARGS], .argc = request_argc,
        // The callee then its arguments are contiguous in the frame the caller copied them to.
        .operands = chained ? args - 1 : NULL, .operand_count = chained ? argc + 1u : 0u,
      };
      GLTANG_Value record = gltang_vm_pending_push(exec, &spec);
      if (record == GLTANG_V_UNWIND || record == exec->roots[GLTANG_ROOT_OOM]) {
        return record;
      }
      ++hs->suspensions;
      exec->request_made = true;
      return GLTANG_V_UNWIND;
    }
    // Nothing to run again (compiled code called this native): the call is nested, and the
    // continuation is called in a scope of its own, in the same record.
    ++hs->nested_requests;
    if (!gltang_v_is_function(extra[N_CALLEE])) {
      return gltang_vm_make_error(exec, GLTANG_ERROR_HOST_FAILED);
    }
    GLTANG_Value nested_args[GLTANG_GUEST_CALL_ARGS_MAX];
    for (uint32_t i = 0; i < request_argc; ++i) {
      nested_args[i] = extra[N_ARGS + i];
    }
    GLTANG_Value answer = gltang_vm_call_guest(exec, extra[N_CALLEE], request_argc, nested_args);
    if (answer == GLTANG_V_UNWIND) {
      return answer;
    }
    extra[N_STATE] = extra[N_NEXT_STATE];
    extra[N_RESULT] = answer;
    continuing = true;
    continuation_index = request_index;
    exec->pending_fuel += gltang_opcode_cost_table[GLTANG_OP_CALL];
  }
}


/**
 * The value of a native itself, once its activation record is open: the
 * engine's own builtins, then a host function (opaque, AD-23: it cannot reach the
 * heap, pause or call guest code through the library's API; one marked resumable is
 * run_resumable_native's). Test natives (src/vm/testnatives.c) are host functions
 * that reach the engine's internals.
 */
static GLTANG_Value run_native(GLTANG_Execution * exec, GLTANG_Value callee, size_t argc, const GLTANG_Value * args) {
  const GLTANG_NativeObject * native = gltang_object(callee);
  switch ((GLTANG_BuiltinId)native->builtin) {
    case GLTANG_BUILTIN_RANDOM_SEEDED:
      if (argc != 1) {
        return gltang_vm_make_error(exec, GLTANG_ERROR_ARGUMENT_COUNT_MISMATCH);
      }
      if (!integer_argument(args[0])) {
        return gltang_vm_make_error(exec, GLTANG_ERROR_INVALID_FUNCTION_CALL);
      }
      return rng_new(exec, (uint64_t)gltang_vm_int(args[0]), false);
    case GLTANG_BUILTIN_RNG_SET_SEED: {
      if (argc != 1) {
        return gltang_vm_make_error(exec, GLTANG_ERROR_ARGUMENT_COUNT_MISMATCH);
      }
      if (!integer_argument(args[0])) {
        return gltang_vm_make_error(exec, GLTANG_ERROR_INVALID_FUNCTION_CALL);
      }
      GLTANG_RngObject * rng = rng_of(native->bound);
      if (rng->is_global) {
        return gltang_vm_make_error(exec, GLTANG_ERROR_GLOBAL_SEED);
      }
      rng->seed = (uint64_t)gltang_vm_int(args[0]);
      gcu_random_mt64_init(&rng->state, rng->seed);
      return native->bound;
    }
    default:
      break;
  }
  const GLTANG_LibraryMember * member = native->member;
  GLTANG_NativeCall call;
  memset(&call, 0, GLTANG_CALL_ZEROED);
  call.exec = exec;
  call.args = args;
  call.argc = argc;
  // The value the native is bound to (gltang_call_bound).
  call.resume_state = native->bound;
  // A native that re-entered guest code may have called another native: the outer
  // one is still running when the inner returns, so the flag goes back as it was.
  const bool outer_in_host = exec->in_host;
  exec->in_host = true;
  bool worked = member->native(&call, member->user);
  exec->in_host = outer_in_host;
  GLTANG_Value result = call_result(exec, &call, worked);
  // Last, so that a nested call the native made does not leave its own status here.
  exec->native_status = call.test_status;
  return result;
}

#define NATIVE_COPY_MAX 16u

/** The refusal of a native's record, as the call's value (the same in both tiers). */
GLTANG_Value gltang_vm_native_refused_value(GLTANG_Execution * exec, GRCORE_Result refused) {
  GRCORE_Context * context = exec->context;
  if (grcore_context_depth(context, GRCORE_DEPTH_NATIVE) >= grcore_context_native_depth(context)) {
    // The native-depth budget is full. The call is not made and its value is the
    // recursion-limit error, as for the guest-depth budget (reference 10.3).
    return gltang_vm_make_error(exec, GLTANG_ERROR_RECURSION_LIMIT);
  }
  // The record could not be allocated: the memory budget's verdict comes first,
  // as for any call.
  if (refused == GRCORE_ERR_LIMIT && GLTANG_NATIVE_POLL(exec, CALL_REFUSED, 0) != GLTANG_ST_OK) {
    return GLTANG_V_UNWIND;
  }
  return exec->roots[GLTANG_ROOT_OOM];
}

/**
 * gltang_vm_call_native for a native that is marked resumable (its object says so in one bit, which the common
 * entry tests): the same wrapper, with the words of a request after the arguments in the record's segment (heap
 * memory, zeroed, so they are defined and pinned), and the mark that tells run_resumable_native whether a request
 * becomes a pending record. A resumable native the interpreter calls (not one compiled code calls, nor the call a
 * record waits for, which is already marked) is called at an instruction.
 */
static GLTANG_Value __attribute__((noinline)) call_resumable_native(GLTANG_Execution * exec, GLTANG_Value callee, size_t argc, const GLTANG_Value * args, bool compiled) {
  const size_t held_count = 1u + argc + N_EXTRA;
  GLTANG_Value * held = gcu_calloc(held_count, sizeof(GLTANG_Value));
  if (!held) {
    if (exec->resume_active) {
      gltang_vm_pending_unconsumed(exec);  // as at the end, below
    }
    return exec->roots[GLTANG_ROOT_OOM];
  }
  held[0] = callee;
  if (argc) {
    memcpy(&held[1], args, argc * sizeof(GLTANG_Value));
  }
  ++exec->natives_called;
  exec->native_status = GLTANG_NATIVE_STATUS_OK;
#ifdef GLTANG_WITH_JIT
  size_t lent = compiled ? gltang_jit_lend_depth(exec) : 0;
#else
  (void)compiled;
#endif
  GRCORE_Stack * stack = grcore_context_stack(exec->context);
  GRCORE_CSegment segment = {(uintptr_t)&held[0], (uintptr_t)&held[0] + held_count * sizeof(GLTANG_Value)};
  GRCORE_ActivationRef record;
  GRCORE_Result entered = grcore_activation_enter(stack, GRCORE_ACTIVATION_NATIVE, exec->engine, false, &segment, &record);
  GLTANG_Value result;
  if (entered != GRCORE_OK) {
#ifdef GLTANG_WITH_JIT
    exec->jit_stats.native_enters_refused += compiled;
#endif
    result = gltang_vm_native_refused_value(exec, entered);
  }
  else {
    if (exec->native_gc_seam) {
      (void)grheap_collect(exec->heap);
    }
    const uint8_t outer_mode = exec->suspend_mode;
    if (!compiled && outer_mode == 0) {
      exec->suspend_mode = 1;
    }
    result = run_resumable_native(exec, held[0], argc, &held[1], &held[1u + argc]);
    exec->suspend_mode = outer_mode;
    (void)grcore_activation_leave(stack, record);
  }
#ifdef GLTANG_WITH_JIT
  gltang_jit_return_depth(exec, lent);
#endif
  gcu_free(held);
  if (__builtin_expect(exec->resume_active, 0)) {
    // Run again to continue from a pending record and refused before run_resumable_native took it (the copy or the
    // record could not be made): the record goes, with the refusal as the call's value.
    gltang_vm_pending_unconsumed(exec);
  }
  return result;
}

GLTANG_Value gltang_vm_call_native(GLTANG_Execution * exec, GLTANG_Value callee, size_t argc, const GLTANG_Value * args, bool compiled) {
  if (__builtin_expect(((const GLTANG_NativeObject *)gltang_object(callee))->builtin & GLTANG_NATIVE_RESUMABLE_BIT, 0)) {
    return call_resumable_native(exec, callee, argc, args, compiled);
  }
  // The callee and the arguments are copied into this frame before anything that
  // can reach a collection: the record below gives exactly the copies as its
  // conservative segment, so what they name is pinned, and the native reads the copy
  // and not the operand stack it came from (which a collection may have moved). The
  // array is cleared first so that every word the collector reads is defined.
  GLTANG_Value held_small[1u + NATIVE_COPY_MAX] = {0};
  GLTANG_Value * held = held_small;
  size_t held_count = 1u + NATIVE_COPY_MAX;
  if (argc > NATIVE_COPY_MAX) {
    // More arguments than the frame holds: the copy is made on the heap, zeroed,
    // and is the record's segment all the same, so every argument count is pinned
    // and read from a copy the operand stack's moves cannot reach.
    held_count = 1u + argc;
    held = gcu_calloc(held_count, sizeof(GLTANG_Value));
    if (!held) {
      return exec->roots[GLTANG_ROOT_OOM];
    }
  }
  held[0] = callee;
  if (argc) {
    memcpy(&held[1], args, argc * sizeof(GLTANG_Value));
  }
  const GLTANG_Value * given = &held[1];
  ++exec->natives_called;
  exec->native_status = GLTANG_NATIVE_STATUS_OK;
#ifdef GLTANG_WITH_JIT
  size_t lent = compiled ? gltang_jit_lend_depth(exec) : 0;
#else
  (void)compiled;
#endif
  GRCORE_Stack * stack = grcore_context_stack(exec->context);
  GRCORE_CSegment segment = {(uintptr_t)&held[0], (uintptr_t)&held[0] + held_count * sizeof(GLTANG_Value)};
  GRCORE_ActivationRef record;
  GRCORE_Result entered = grcore_activation_enter(stack, GRCORE_ACTIVATION_NATIVE, exec->engine, false, &segment, &record);
  GLTANG_Value result;
  if (entered != GRCORE_OK) {
#ifdef GLTANG_WITH_JIT
    exec->jit_stats.native_enters_refused += compiled;
#endif
    result = gltang_vm_native_refused_value(exec, entered);
  }
  else {
    if (exec->native_gc_seam) {
      // Entering a nested activation is a GC point (AD-17); the heap's torture
      // mode owns only the ones the heap has, so under torture the engine makes
      // this one. The native then finds its arguments where they were copied.
      (void)grheap_collect(exec->heap);
    }
    result = run_native(exec, held[0], argc, given);
    (void)grcore_activation_leave(stack, record);
  }
#ifdef GLTANG_WITH_JIT
  gltang_jit_return_depth(exec, lent);
#endif
  if (held != held_small) {
    gcu_free(held);
  }
  return result;
}

/*
 * For the tests only: how many natives the execution has entered through the shared
 * wrapper, from either tier. Declared in vm/test_hooks.h.
 */
uint64_t gltang_vm_test_natives_called(const GLTANG_Execution * execution) {
  return execution ? execution->natives_called : 0;
}

/*
 * For the tests only: the position the execution last recorded (the interpreter's
 * SYNC, compiled code's stored site), which a later error with no SYNC of its own
 * would take as its origin. Declared in vm/test_hooks.h.
 */
uint64_t gltang_vm_test_position(const GLTANG_Execution * execution) {
  return execution ? ((uint64_t)execution->current_function << 32) | execution->current_offset : 0;
}

/*
 * For the tests only: how many temporaries the execution has storage for (0 until its first push
 * allocates it). Declared in vm/test_hooks.h.
 */
uint64_t gltang_vm_test_temp_capacity(const GLTANG_Execution * execution) {
  return execution ? (uint64_t)execution->temp_capacity : 0;
}

/*
 * For the tests only: whether a slot or variable word (of kind VALUE) is a float
 * and, if so, its 64 bits. The frame differential compares floats by these bits,
 * which the inspect text (six decimals, every NaN as nan) does not carry.
 * Declared in vm/test_hooks.h.
 */
bool gltang_vm_test_float_bits(uint64_t value, uint64_t * bits) {
  GLTANG_Value v = (GLTANG_Value)value;
  if (gltang_vm_kind(v) != GLTANG_KIND_FLOAT) {
    return false;
  }
  double d = gltang_vm_float(v);
  memcpy(bits, &d, sizeof(d));
  return true;
}

