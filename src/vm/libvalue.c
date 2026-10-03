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

#include <ghoti.io/lang-tang/macros.h>

#include <string.h>
#include <ghoti.io/cutil/memory.h>
#include "vm_internal.h"

// ---------------------------------------------------------------------------
// The heap types
// ---------------------------------------------------------------------------

static void native_trace(GRHEAP_Tracer * tracer, void * payload) {
  GLTANG_NativeObject * native = payload;
  grheap_trace_word(tracer, &native->bound);
}

const GRHEAP_Type gltang_type_library = {"lang-tang library", sizeof(GLTANG_LibraryObject), NULL, NULL, NULL};
const GRHEAP_Type gltang_type_native = {"lang-tang native function", sizeof(GLTANG_NativeObject), native_trace, NULL, NULL};
const GRHEAP_Type gltang_type_template = {"lang-tang template", sizeof(GLTANG_TemplateObject), NULL, NULL, NULL};
const GRHEAP_Type gltang_type_rng = {"lang-tang generator", sizeof(GLTANG_RngObject), NULL, NULL, NULL};

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

static GLTANG_Value native_value(GLTANG_Execution * exec, const GLTANG_LibraryMember * member, GLTANG_BuiltinId builtin, GLTANG_Value bound) {
  void * object;
  GLTANG_Status st = gltang_vm_alloc(exec, &gltang_type_native, sizeof(GLTANG_NativeObject), &object);
  if (st != GLTANG_ST_OK) {
    return gltang_vm_failure_value(exec, st);
  }
  GLTANG_NativeObject * value = object;
  value->kind = GLTANG_OBJ_NATIVE;
  value->builtin = (uint32_t)builtin;
  value->member = member;
  (void)grheap_store_word(exec->heap, value, &value->bound, bound);
  return gltang_value_of(value);
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

GLTANG_Value gltang_vm_rng_attr(GLTANG_Execution * exec, GLTANG_Value rng, const char * name, size_t length) {
  GLTANG_RngObject * state = rng_of(rng);
  if (length == 8 && !memcmp(name, "next_int", 8)) {
    return gltang_vm_make_int(exec, (int64_t)gcu_random_mt64_next(&state->state));
  }
  if (length == 10 && !memcmp(name, "next_float", 10)) {
    uint64_t word = gcu_random_mt64_next(&state->state);
    return gltang_vm_make_float(exec, (double)(word >> 11) * (1.0 / 9007199254740992.0));
  }
  if (length == 9 && !memcmp(name, "next_bool", 9)) {
    return gltang_v_from_bool((gcu_random_mt64_next(&state->state) & 1u) != 0);
  }
  if (length == 8 && !memcmp(name, "set_seed", 8)) {
    return native_value(exec, NULL, GLTANG_BUILTIN_RNG_SET_SEED, rng);
  }
  return gltang_vm_make_error(exec, GLTANG_ERROR_NOT_IMPLEMENTED);
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
      exec->in_host = true;
      bool available = member->factory(member->user, &host);
      exec->in_host = false;
      return available ? host_value(exec, &host) : GLTANG_V_NULL;
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
  // program's, then the built-ins; the first that has it wins.
  const GLTANG_Library * layers[3] = {
    exec->libraries,
    gltang_exec_current_program(exec)->libraries,
    gltang_library_builtins(),
  };
  const GLTANG_LibraryMember * member = NULL;
  for (size_t i = 0; i < 3 && !member; ++i) {
    member = gltang_library_find(layers[i], text, first);
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

static void call_clear(GLTANG_NativeCall * call) {
  gcu_free(call->text);
  call->text = NULL;
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
  call->failed_to_copy = (unsigned)encoding > (unsigned)GLTANG_UNICODE_STRING_TYPE_JAVASCRIPT;
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

void gltang_call_return_error(GLTANG_NativeCall * call, GLTANG_ErrorKind kind) {
  if (call) {
    call_clear(call);
    call->set = GLTANG_CALL_ERROR;
    call->error = kind;
  }
}

/** The value a finished host call stands for. */
static GLTANG_Value call_result(GLTANG_Execution * exec, GLTANG_NativeCall * call, bool worked) {
  GLTANG_Value result;
  if (call->set == GLTANG_CALL_ERROR) {
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
  call_clear(call);
  return result;
}

static bool integer_argument(GLTANG_Value v) {
  return gltang_vm_kind(v) == GLTANG_KIND_INTEGER;
}

GLTANG_Value gltang_vm_call_native(GLTANG_Execution * exec, GLTANG_Value callee, size_t argc, const GLTANG_Value * args) {
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
  GLTANG_NativeCall call;
  memset(&call, 0, sizeof(call));
  call.exec = exec;
  call.args = args;
  call.argc = argc;
  exec->in_host = true;
  bool worked = native->member->native(&call, native->member->user);
  exec->in_host = false;
  return call_result(exec, &call, worked);
}
