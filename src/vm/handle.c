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
 * Handles (handle.h): how a host function touches the program's values while the
 * program runs.
 *
 * A **scope** is a window onto the execution's temporary roots (`temps`), which
 * the collector already reads as precise slots and rewrites when it moves an
 * object. The first handle call of a callback opens one: it records where the
 * window starts, takes the next serial, and remembers the scope it replaces. A
 * handle is the serial and the position in the window. Closing the scope drops
 * the window and puts the outer scope back; nothing else has to be done for a
 * handle to survive a collection, which is the point of building it on `temps`.
 *
 * Nothing in the window is ever held by a C variable across a call that can
 * collect: a constructor reads the values it needs out of the handle slots after
 * its allocation, and puts a result it has not yet given a handle to on `temps`
 * (above the window) until it has.
 *
 * A **persistent handle** is the same idea in a table of the execution's that
 * outlives callbacks; its slots are reported to the collector as roots.
 */

#include <ghoti.io/lang-tang/macros.h>

#include <string.h>
#include "vm_internal.h"

#define SCOPE_LIMIT_MAX ((size_t)1 << 20)
#define PERSISTENT_LIMIT_MAX ((size_t)1 << 20)

// ---------------------------------------------------------------------------
// Scopes
// ---------------------------------------------------------------------------

/** The handle state of an execution, made the first time it is wanted; NULL when memory is out. */
static GLTANG_HandleState * state_of(GLTANG_Execution * exec) {
  if (!exec->handles) {
    exec->handles = gcu_allocator_calloc(exec->allocator, 1, sizeof(GLTANG_HandleState));
    if (exec->handles) {
      exec->handles->scope_limit = GLTANG_SCOPE_LIMIT_DEFAULT;
      exec->handles->persistent_limit = GLTANG_PERSISTENT_LIMIT_DEFAULT;
    }
  }
  return exec->handles;
}

GLTANG_HandleState * gltang_vm_handle_state(GLTANG_Execution * exec) {
  return state_of(exec);
}

static bool frame_push(GLTANG_Execution * exec) {
  GLTANG_HandleState * hs = state_of(exec);
  if (!hs) {
    return false;
  }
  if (hs->scope_serial == UINT32_MAX) {
    return false;
  }
  if (hs->frame_count == hs->frame_capacity) {
    size_t grown = hs->frame_capacity ? hs->frame_capacity * 2u : 8u;
    GLTANG_HostScope * bigger = gcu_allocator_realloc(exec->allocator, hs->frames, grown * sizeof(*bigger));
    if (!bigger) {
      return false;
    }
    hs->frames = bigger;
    hs->frame_capacity = grown;
  }
  GLTANG_HostScope * f = &hs->frames[hs->frame_count++];
  f->serial = ++hs->scope_serial;
  f->base = exec->temp_count;
  f->count = 0;
  return true;
}

static bool scope_ensure(GLTANG_NativeCall * call) {
  if (call->scope) {
    return true;
  }
  GLTANG_Execution * exec = call->exec;
  size_t floor = exec->handles ? exec->handles->frame_count : 0;
  if (!frame_push(exec)) {
    return false;
  }
  GLTANG_HandleState * hs = exec->handles;
  call->frame_floor = floor;
  call->scope = hs->frames[floor].serial;
  return true;
}

void gltang_vm_scope_close(GLTANG_NativeCall * call) {
  if (!call->scope) {
    return;
  }
  GLTANG_Execution * exec = call->exec;
  GLTANG_HandleState * hs = exec->handles;
  if (hs->frame_count > call->frame_floor) {
    gltang_vm_temp_release(exec, hs->frames[call->frame_floor].base);
    hs->frame_count = call->frame_floor;
  }
  call->scope = 0;
}

/** The frame of this call that the handle belongs to, or NULL. */
static const GLTANG_HostScope * frame_of(const GLTANG_NativeCall * call, GLTANG_Handle handle) {
  if (!call || !call->scope || !handle.scope) {
    return NULL;
  }
  const GLTANG_HandleState * hs = call->exec->handles;
  for (size_t i = hs->frame_count; i > call->frame_floor; --i) {
    const GLTANG_HostScope * f = &hs->frames[i - 1u];
    if (f->serial == handle.scope) {
      return handle.index < f->count ? f : NULL;
    }
  }
  return NULL;
}

static bool handle_ok(const GLTANG_NativeCall * call, GLTANG_Handle handle) {
  return frame_of(call, handle) != NULL;
}

GLTANG_Value gltang_vm_handle_value(const GLTANG_NativeCall * call, GLTANG_Handle handle, bool * ok) {
  const GLTANG_HostScope * f = frame_of(call, handle);
  if (!f) {
    *ok = false;
    return GLTANG_V_NULL;
  }
  *ok = true;
  return call->exec->temps[f->base + handle.index];
}

/** Gives `v` a handle in the innermost frame. A scope that is full, or memory that is out, is the callback's out-of-memory result. */
static GLTANG_Result push_handle(GLTANG_NativeCall * call, GLTANG_Value v, GLTANG_Handle * out) {
  GLTANG_Execution * exec = call->exec;
  if (!scope_ensure(call)) {
    call->scope_oom = true;
    return GLTANG_ERR_LIMIT;
  }
  GLTANG_HandleState * hs = exec->handles;
  GLTANG_HostScope * top = &hs->frames[hs->frame_count - 1u];
  // The frames are contiguous: nothing else is on the temporaries above them between calls.
  assert(exec->temp_count == top->base + top->count);
  const size_t live = exec->temp_count - hs->frames[call->frame_floor].base;
  if (live >= hs->scope_limit || !gltang_vm_temp_push(exec, v)) {
    call->scope_oom = true;
    return GLTANG_ERR_LIMIT;
  }
  out->scope = top->serial;
  out->index = (uint32_t)top->count++;
  return GLTANG_OK;
}

GLTANG_Result gltang_handle_push_frame(GLTANG_NativeCall * call) {
  if (!call) {
    return GLTANG_ERR_INVALID;
  }
  if (!scope_ensure(call) || !frame_push(call->exec)) {
    call->scope_oom = true;
    return GLTANG_ERR_LIMIT;
  }
  return GLTANG_OK;
}

GLTANG_Result gltang_handle_pop_frame(GLTANG_NativeCall * call) {
  if (!call || !call->scope || call->exec->handles->frame_count <= call->frame_floor + 1u) {
    return GLTANG_ERR_INVALID;   // none pushed by this call: the first frame is the scope itself
  }
  GLTANG_Execution * exec = call->exec;
  GLTANG_HandleState * hs = exec->handles;
  GLTANG_HostScope * top = &hs->frames[hs->frame_count - 1u];
  gltang_vm_temp_release(exec, top->base);
  --hs->frame_count;
  return GLTANG_OK;
}

/** What a failed allocation answered with becomes the callback's result. */
static GLTANG_Result failed_value(GLTANG_NativeCall * call, GLTANG_Value v) {
  if (v == GLTANG_V_UNWIND) {
    call->scope_unwind = true;
    return GLTANG_ERR_LIMIT;
  }
  if (v == call->exec->roots[GLTANG_ROOT_OOM]) {
    call->scope_oom = true;
    return GLTANG_ERR_LIMIT;
  }
  return GLTANG_OK;
}

static bool is_failure(const GLTANG_NativeCall * call, GLTANG_Value v) {
  return v == GLTANG_V_UNWIND || v == call->exec->roots[GLTANG_ROOT_OOM];
}

// ---------------------------------------------------------------------------
// Arguments, the bound value, the answer
// ---------------------------------------------------------------------------

GLTANG_Result gltang_call_arg(GLTANG_NativeCall * call, size_t index, GLTANG_Handle * out) {
  if (!call || !out || index >= call->argc) {
    return GLTANG_ERR_INVALID;
  }
  GLTANG_Handle h;
  GLTANG_Result r = push_handle(call, call->args[index], &h);
  if (r == GLTANG_OK) {
    *out = h;
  }
  return r;
}

void * gltang_call_host_context(const GLTANG_NativeCall * call) {
  return call && call->exec ? call->exec->host_context : NULL;
}

GLTANG_Result gltang_call_bound(GLTANG_NativeCall * call, GLTANG_Handle * out) {
  if (!call || !out) {
    return GLTANG_ERR_INVALID;
  }
  GLTANG_Handle h;
  GLTANG_Result r = push_handle(call, call->resume_state, &h);
  if (r == GLTANG_OK) {
    *out = h;
  }
  return r;
}

// ---------------------------------------------------------------------------
// Readers
// ---------------------------------------------------------------------------

bool gltang_handle_valid(const GLTANG_NativeCall * call, GLTANG_Handle handle) {
  return handle_ok(call, handle);
}

static GLTANG_Value read(const GLTANG_NativeCall * call, GLTANG_Handle handle, GLTANG_ValueKind kind, bool * ok) {
  GLTANG_Value v = gltang_vm_handle_value(call, handle, ok);
  if (*ok && gltang_vm_kind(v) != kind) {
    *ok = false;
  }
  return v;
}

GLTANG_ValueKind gltang_handle_kind(const GLTANG_NativeCall * call, GLTANG_Handle handle) {
  bool ok;
  GLTANG_Value v = gltang_vm_handle_value(call, handle, &ok);
  return ok ? gltang_vm_kind(v) : GLTANG_KIND_NULL;
}

bool gltang_handle_bool(const GLTANG_NativeCall * call, GLTANG_Handle handle) {
  bool ok;
  GLTANG_Value v = read(call, handle, GLTANG_KIND_BOOL, &ok);
  return ok && v == GLTANG_V_TRUE;
}

int64_t gltang_handle_integer(const GLTANG_NativeCall * call, GLTANG_Handle handle) {
  bool ok;
  GLTANG_Value v = read(call, handle, GLTANG_KIND_INTEGER, &ok);
  return ok ? gltang_vm_int(v) : 0;
}

double gltang_handle_float(const GLTANG_NativeCall * call, GLTANG_Handle handle) {
  bool ok;
  GLTANG_Value v = read(call, handle, GLTANG_KIND_FLOAT, &ok);
  return ok ? gltang_vm_float(v) : 0.0;
}

const char * gltang_handle_text(const GLTANG_NativeCall * call, GLTANG_Handle handle, size_t * out_length) {
  bool ok;
  GLTANG_Value v = read(call, handle, GLTANG_KIND_STRING, &ok);
  if (!ok) {
    return NULL;
  }
  const GLTANG_StringBlock * s = gltang_vm_string(v);
  if (out_length) {
    *out_length = (size_t)s->byte_length;
  }
  return gltang_string_bytes(s);
}

size_t gltang_handle_copy_text(const GLTANG_NativeCall * call, GLTANG_Handle handle, char * buffer, size_t capacity) {
  bool ok;
  GLTANG_Value v = read(call, handle, GLTANG_KIND_STRING, &ok);
  if (!ok) {
    if (buffer && capacity) {
      buffer[0] = '\0';
    }
    return 0;
  }
  const GLTANG_StringBlock * s = gltang_vm_string(v);
  size_t length = (size_t)s->byte_length;
  if (buffer && capacity) {
    size_t n = length < capacity - 1u ? length : capacity - 1u;
    memcpy(buffer, gltang_string_bytes(s), n);
    buffer[n] = '\0';
  }
  return length;
}

size_t gltang_handle_string_graphemes(const GLTANG_NativeCall * call, GLTANG_Handle handle) {
  bool ok;
  GLTANG_Value v = read(call, handle, GLTANG_KIND_STRING, &ok);
  return ok ? (size_t)gltang_vm_string(v)->grapheme_length : 0;
}

size_t gltang_handle_string_segments(const GLTANG_NativeCall * call, GLTANG_Handle handle) {
  bool ok;
  GLTANG_Value v = read(call, handle, GLTANG_KIND_STRING, &ok);
  return ok ? (size_t)gltang_vm_string(v)->segment_count : 0;
}

bool gltang_handle_string_segment(const GLTANG_NativeCall * call, GLTANG_Handle handle, size_t index, GLTANG_StringSegment * out) {
  bool ok;
  GLTANG_Value v = read(call, handle, GLTANG_KIND_STRING, &ok);
  if (!ok || !out || out->size != sizeof(GLTANG_StringSegment)) {
    return false;
  }
  const GLTANG_StringBlock * s = gltang_vm_string(v);
  if (index >= s->segment_count) {
    return false;
  }
  const uint64_t * segments = gltang_string_segments(s);
  size_t first = GLTANG_SEGMENT_FIRST(segments[index]);
  size_t next = index + 1u < s->segment_count ? GLTANG_SEGMENT_FIRST(segments[index + 1u]) : (size_t)s->grapheme_length;
  size_t begin = gltang_string_byte_offset(s, first);
  size_t end = gltang_string_byte_offset(s, next);
  out->offset = begin;
  out->length = end - begin;
  out->encoding = GLTANG_SEGMENT_TYPE(segments[index]);
  return true;
}

size_t gltang_handle_array_length(const GLTANG_NativeCall * call, GLTANG_Handle handle) {
  bool ok;
  GLTANG_Value v = read(call, handle, GLTANG_KIND_ARRAY, &ok);
  return ok ? (size_t)gltang_vm_array(v)->length : 0;
}

GLTANG_Result gltang_handle_array_get(GLTANG_NativeCall * call, GLTANG_Handle array, size_t index, GLTANG_Handle * out) {
  bool ok;
  GLTANG_Value v = read(call, array, GLTANG_KIND_ARRAY, &ok);
  if (!ok || !out || index >= gltang_vm_array(v)->length) {
    return GLTANG_ERR_INVALID;
  }
  GLTANG_Handle h;
  GLTANG_Result r = push_handle(call, gltang_vm_array(v)->store.typed->slots[index], &h);
  if (r == GLTANG_OK) {
    *out = h;
  }
  return r;
}

size_t gltang_handle_map_count(const GLTANG_NativeCall * call, GLTANG_Handle handle) {
  bool ok;
  GLTANG_Value v = read(call, handle, GLTANG_KIND_MAP, &ok);
  return ok ? (size_t)gltang_vm_map(v)->count : 0;
}

GLTANG_Result gltang_handle_map_get(GLTANG_NativeCall * call, GLTANG_Handle map, const char * key, size_t key_length, bool * found, GLTANG_Handle * out) {
  bool ok;
  GLTANG_Value v = read(call, map, GLTANG_KIND_MAP, &ok);
  if (!ok || !out || (!key && key_length)) {
    return GLTANG_ERR_INVALID;
  }
  bool present;
  GLTANG_Value member = gltang_vm_map_get(v, key ? key : "", key_length, &present);
  if (found) {
    *found = present;
  }
  if (!present) {
    return GLTANG_OK;
  }
  GLTANG_Handle h;
  GLTANG_Result r = push_handle(call, member, &h);
  if (r == GLTANG_OK) {
    *out = h;
  }
  return r;
}

static GLTANG_Result map_entry(GLTANG_NativeCall * call, GLTANG_Handle map, size_t index, size_t part, GLTANG_Handle * out) {
  bool ok;
  GLTANG_Value v = read(call, map, GLTANG_KIND_MAP, &ok);
  if (!ok || !out || index >= gltang_vm_map(v)->count) {
    return GLTANG_ERR_INVALID;
  }
  GLTANG_Handle h;
  GLTANG_Result r = push_handle(call, gltang_vm_map(v)->store.typed->slots[2u * index + part], &h);
  if (r == GLTANG_OK) {
    *out = h;
  }
  return r;
}

GLTANG_Result gltang_handle_map_key_at(GLTANG_NativeCall * call, GLTANG_Handle map, size_t index, GLTANG_Handle * out) {
  return map_entry(call, map, index, 0, out);
}

GLTANG_Result gltang_handle_map_value_at(GLTANG_NativeCall * call, GLTANG_Handle map, size_t index, GLTANG_Handle * out) {
  return map_entry(call, map, index, 1, out);
}

// ---------------------------------------------------------------------------
// Constructors
// ---------------------------------------------------------------------------

/** Gives a value that was just made a handle, or turns a failed allocation into the callback's result. */
static GLTANG_Result made(GLTANG_NativeCall * call, GLTANG_Value v, GLTANG_Handle * out) {
  if (is_failure(call, v)) {
    return failed_value(call, v);
  }
  GLTANG_Handle h;
  GLTANG_Result r = push_handle(call, v, &h);
  if (r == GLTANG_OK) {
    *out = h;
  }
  return r;
}

GLTANG_Result gltang_handle_make_null(GLTANG_NativeCall * call, GLTANG_Handle * out) {
  return call && out ? made(call, GLTANG_V_NULL, out) : GLTANG_ERR_INVALID;
}

GLTANG_Result gltang_handle_make_bool(GLTANG_NativeCall * call, bool value, GLTANG_Handle * out) {
  return call && out ? made(call, gltang_v_from_bool(value), out) : GLTANG_ERR_INVALID;
}

GLTANG_Result gltang_handle_make_integer(GLTANG_NativeCall * call, int64_t value, GLTANG_Handle * out) {
  return call && out ? made(call, gltang_vm_make_int(call->exec, value), out) : GLTANG_ERR_INVALID;
}

GLTANG_Result gltang_handle_make_float(GLTANG_NativeCall * call, double value, GLTANG_Handle * out) {
  return call && out ? made(call, gltang_vm_make_float(call->exec, value), out) : GLTANG_ERR_INVALID;
}

GLTANG_Result gltang_handle_make_string(GLTANG_NativeCall * call, const char * text, size_t length, GLTANG_String_Type encoding, GLTANG_Handle * out) {
  if (!call || !out || (!text && length) || (unsigned)encoding > (unsigned)GLTANG_UNICODE_STRING_TYPE_JAVASCRIPT) {
    return GLTANG_ERR_INVALID;
  }
  GLTANG_Value v = gltang_vm_string_from_utf8(call->exec, text ? text : "", length, encoding);
  if (v == GLTANG_V_NULL) {
    // Text that is not UTF-8 (the engine answers null for it).
    return GLTANG_ERR_INVALID;
  }
  return made(call, v, out);
}

GLTANG_Result gltang_handle_make_concat(GLTANG_NativeCall * call, GLTANG_Handle first, GLTANG_Handle second, GLTANG_Handle * out) {
  bool ok_a, ok_b;
  GLTANG_Value a = read(call, first, GLTANG_KIND_STRING, &ok_a);
  GLTANG_Value b = read(call, second, GLTANG_KIND_STRING, &ok_b);
  if (!ok_a || !ok_b || !out) {
    return GLTANG_ERR_INVALID;
  }
  return made(call, gltang_vm_string_concat(call->exec, a, b), out);
}

GLTANG_Result gltang_handle_make_array(GLTANG_NativeCall * call, size_t capacity, GLTANG_Handle * out) {
  if (!call || !out) {
    return GLTANG_ERR_INVALID;
  }
  return made(call, gltang_vm_array_new(call->exec, capacity), out);
}

GLTANG_Result gltang_handle_make_map(GLTANG_NativeCall * call, size_t capacity, GLTANG_Handle * out) {
  if (!call || !out) {
    return GLTANG_ERR_INVALID;
  }
  return made(call, gltang_vm_map_new(call->exec, capacity), out);
}

/**
 * The copy of `value` that is stored into a container (the language copies a
 * container it stores), held on the temporaries above the window until the
 * caller has stored it. Returns the mark to release to, or SIZE_MAX with the
 * callback's result set.
 */
static size_t hold_stored_copy(GLTANG_NativeCall * call, GLTANG_Value value, GLTANG_Result * r) {
  GLTANG_Execution * exec = call->exec;
  size_t mark = gltang_vm_temp_mark(exec);
  GLTANG_Value copy = gltang_vm_op_adopt(exec, value);
  if (is_failure(call, copy)) {
    *r = failed_value(call, copy);
    return SIZE_MAX;
  }
  if (!gltang_vm_temp_push(exec, copy)) {
    call->scope_oom = true;
    *r = GLTANG_ERR_LIMIT;
    return SIZE_MAX;
  }
  *r = GLTANG_OK;
  return mark;
}

GLTANG_Result gltang_handle_array_push(GLTANG_NativeCall * call, GLTANG_Handle array, GLTANG_Handle element) {
  bool ok_a, ok_e;
  GLTANG_Value a = read(call, array, GLTANG_KIND_ARRAY, &ok_a);
  GLTANG_Value e = gltang_vm_handle_value(call, element, &ok_e);
  if (!ok_a || !ok_e) {
    return GLTANG_ERR_INVALID;
  }
  GLTANG_Execution * exec = call->exec;
  GLTANG_Result r;
  size_t mark = hold_stored_copy(call, e, &r);
  if (mark == SIZE_MAX) {
    return r;
  }
  // The array is read again from its handle after every call that can collect.
  a = gltang_vm_handle_value(call, array, &ok_a);
  size_t length = (size_t)gltang_vm_array(a)->length;
  GLTANG_Value grown = gltang_vm_array_grow(exec, a, length + 1u);
  if (is_failure(call, grown)) {
    gltang_vm_temp_release(exec, mark);
    return failed_value(call, grown);
  }
  a = gltang_vm_handle_value(call, array, &ok_a);
  gltang_vm_array_set(exec, a, length, gltang_vm_temp_at(exec, mark));
  gltang_vm_temp_release(exec, mark);
  return GLTANG_OK;
}

GLTANG_Result gltang_handle_map_set(GLTANG_NativeCall * call, GLTANG_Handle map, const char * key, size_t key_length, GLTANG_String_Type key_encoding, GLTANG_Handle value) {
  bool ok_m, ok_v;
  (void)read(call, map, GLTANG_KIND_MAP, &ok_m);
  GLTANG_Value v = gltang_vm_handle_value(call, value, &ok_v);
  if (!ok_m || !ok_v || (!key && key_length) || (unsigned)key_encoding > (unsigned)GLTANG_UNICODE_STRING_TYPE_JAVASCRIPT) {
    return GLTANG_ERR_INVALID;
  }
  GLTANG_Execution * exec = call->exec;
  GLTANG_Result r;
  size_t mark = hold_stored_copy(call, v, &r);
  if (mark == SIZE_MAX) {
    return r;
  }
  GLTANG_Value k = gltang_vm_string_from_utf8(exec, key ? key : "", key_length, key_encoding);
  if (k == GLTANG_V_NULL) {
    gltang_vm_temp_release(exec, mark);
    return GLTANG_ERR_INVALID;
  }
  if (is_failure(call, k)) {
    gltang_vm_temp_release(exec, mark);
    return failed_value(call, k);
  }
  if (!gltang_vm_temp_push(exec, k)) {
    gltang_vm_temp_release(exec, mark);
    call->scope_oom = true;
    return GLTANG_ERR_LIMIT;
  }
  // Every value is read again from where it is held: the copy and the key from
  // the temporaries, the map from its handle.
  GLTANG_Value m = gltang_vm_handle_value(call, map, &ok_m);
  GLTANG_Value set = gltang_vm_map_set(exec, m, gltang_vm_temp_at(exec, mark + 1u), gltang_vm_temp_at(exec, mark));
  gltang_vm_temp_release(exec, mark);
  if (is_failure(call, set)) {
    return failed_value(call, set);
  }
  return GLTANG_OK;
}

/** Remembers a library the execution has to keep alive; from its allocator, so it is charged. */
static bool keep_library(GLTANG_Execution * exec, GLTANG_Library * library) {
  GLTANG_HandleState * hs = state_of(exec);
  if (!hs) {
    return false;
  }
  if (hs->held_library_count == hs->held_library_capacity) {
    size_t grown = hs->held_library_capacity ? hs->held_library_capacity * 2u : 8u;
    GLTANG_Library ** bigger = gcu_allocator_realloc(exec->allocator, hs->held_libraries, grown * sizeof(*bigger));
    if (!bigger) {
      return false;
    }
    hs->held_libraries = bigger;
    hs->held_library_capacity = grown;
  }
  hs->held_libraries[hs->held_library_count++] = library;
  return true;
}

/** Remembers a native record the execution owns. */
static bool keep_member(GLTANG_Execution * exec, GLTANG_LibraryMember * member) {
  GLTANG_HandleState * hs = state_of(exec);
  if (!hs) {
    return false;
  }
  if (hs->owned_member_count == hs->owned_member_capacity) {
    size_t grown = hs->owned_member_capacity ? hs->owned_member_capacity * 2u : 8u;
    GLTANG_LibraryMember ** bigger = gcu_allocator_realloc(exec->allocator, hs->owned_members, grown * sizeof(*bigger));
    if (!bigger) {
      return false;
    }
    hs->owned_members = bigger;
    hs->owned_member_capacity = grown;
  }
  hs->owned_members[hs->owned_member_count++] = member;
  return true;
}

GLTANG_Result gltang_handle_make_library(GLTANG_NativeCall * call, GLTANG_Library * library, GLTANG_Handle * out) {
  if (!call || !library || !out) {
    return GLTANG_ERR_INVALID;
  }
  GLTANG_Execution * exec = call->exec;
  bool held = false;
  const GLTANG_HandleState * seen = exec->handles;
  for (size_t i = 0; seen && i < seen->held_library_count; ++i) {
    held = held || seen->held_libraries[i] == library;
  }
  if (!held) {
    GLTANG_Library * attached = gltang_library_attach(library);
    if (!attached || !keep_library(exec, attached)) {
      gltang_library_release(attached);
      call->scope_oom = true;
      return GLTANG_ERR_LIMIT;
    }
  }
  return made(call, gltang_vm_make_library_value(exec, library), out);
}

GLTANG_Result gltang_handle_make_native(GLTANG_NativeCall * call, GLTANG_NativeFn function, void * user, GLTANG_Handle bound, GLTANG_Handle * out) {
  if (!call || !function || !out) {
    return GLTANG_ERR_INVALID;
  }
  GLTANG_Execution * exec = call->exec;
  GLTANG_Value b = GLTANG_V_NULL;
  size_t mark = gltang_vm_temp_mark(exec);
  if (bound.scope || bound.index) {
    bool ok;
    GLTANG_Value v = gltang_vm_handle_value(call, bound, &ok);
    if (!ok) {
      return GLTANG_ERR_INVALID;
    }
    GLTANG_Result r;
    mark = hold_stored_copy(call, v, &r);
    if (mark == SIZE_MAX) {
      return r;
    }
    b = gltang_vm_temp_at(exec, mark);
  }
  GLTANG_LibraryMember * member = gcu_allocator_calloc(exec->allocator, 1, sizeof(GLTANG_LibraryMember));
  if (!member || !keep_member(exec, member)) {
    gcu_allocator_free(exec->allocator, member);
    gltang_vm_temp_release(exec, mark);
    call->scope_oom = true;
    return GLTANG_ERR_LIMIT;
  }
  member->kind = GLTANG_MEMBER_NATIVE;
  member->native = function;
  member->user = user;
  // The bound value waits on the temporaries across the allocation, which the
  // constructor of the native object takes care of itself.
  GLTANG_Value native = gltang_vm_make_native_value(exec, member, b);
  gltang_vm_temp_release(exec, mark);
  return made(call, native, out);
}

// ---------------------------------------------------------------------------
// Custom values
// ---------------------------------------------------------------------------

GLTANG_Result gltang_handle_make_custom(GLTANG_NativeCall * call, const GLTANG_Type * type, GLTANG_Handle * out) {
  if (!call || !type || !out) {
    return GLTANG_ERR_INVALID;
  }
  // Only a type of a registry this execution attached: its registry is then
  // retained for as long as an instance can exist (AD-22).
  if (!gltang_vm_type_attached(call->exec, type)) {
    return GLTANG_ERR_INVALID;
  }
  return made(call, gltang_vm_make_custom(call->exec, type), out);
}

/** The custom value a handle names, or NULL. */
static GLTANG_CustomObject * custom_of(const GLTANG_NativeCall * call, GLTANG_Handle handle) {
  bool ok;
  GLTANG_Value v = gltang_vm_handle_value(call, handle, &ok);
  return ok && gltang_v_is_custom(v) ? gltang_vm_custom(v) : NULL;
}

const GLTANG_Type * gltang_handle_custom_type(const GLTANG_NativeCall * call, GLTANG_Handle handle) {
  const GLTANG_CustomObject * custom = custom_of(call, handle);
  return custom ? custom->type : NULL;
}

GLTANG_Result gltang_handle_custom_slot_get(GLTANG_NativeCall * call, GLTANG_Handle handle, size_t index, GLTANG_Handle * out) {
  const GLTANG_CustomObject * custom = custom_of(call, handle);
  if (!custom || !out || index >= custom->slot_count) {
    return GLTANG_ERR_INVALID;
  }
  GLTANG_Handle h;
  GLTANG_Result r = push_handle(call, custom->slots[index], &h);
  if (r == GLTANG_OK) {
    *out = h;
  }
  return r;
}

GLTANG_Result gltang_handle_custom_slot_set(GLTANG_NativeCall * call, GLTANG_Handle handle, size_t index, GLTANG_Handle value) {
  const GLTANG_CustomObject * custom = custom_of(call, handle);
  bool ok_v;
  GLTANG_Value v = gltang_vm_handle_value(call, value, &ok_v);
  if (!custom || !ok_v || index >= custom->slot_count) {
    return GLTANG_ERR_INVALID;
  }
  // A container is copied as every store of one is; the copy is a GC point, so
  // the instance is read again from its handle after it.
  GLTANG_Result r;
  size_t mark = hold_stored_copy(call, v, &r);
  if (mark == SIZE_MAX) {
    return r;
  }
  GLTANG_CustomObject * holder = custom_of(call, handle);
  // A slot is traced, so the store goes through the barrier (AD-11).
  (void)grheap_store_word(call->exec->heap, holder, &holder->slots[index], gltang_vm_temp_at(call->exec, mark));
  gltang_vm_temp_release(call->exec, mark);
  return GLTANG_OK;
}

/** Whether `[offset, offset + length)` is inside an instance's raw bytes. */
static bool byte_range(const GLTANG_CustomObject * custom, size_t offset, size_t length) {
  size_t size = custom->type->descriptor.byte_count;
  return offset <= size && length <= size - offset;
}

GLTANG_Result gltang_handle_custom_read_bytes(const GLTANG_NativeCall * call, GLTANG_Handle handle, size_t offset, void * buffer, size_t length) {
  GLTANG_CustomObject * custom = custom_of(call, handle);
  if (!custom || (!buffer && length) || !byte_range(custom, offset, length)) {
    return GLTANG_ERR_INVALID;
  }
  if (length) {
    memcpy(buffer, gltang_custom_bytes(custom) + offset, length);
  }
  return GLTANG_OK;
}

GLTANG_Result gltang_handle_custom_write_bytes(GLTANG_NativeCall * call, GLTANG_Handle handle, size_t offset, const void * bytes, size_t length) {
  GLTANG_CustomObject * custom = custom_of(call, handle);
  if (!custom || (!bytes && length) || !byte_range(custom, offset, length)) {
    return GLTANG_ERR_INVALID;
  }
  // Raw bytes are never traced, so no barrier: they are plain data.
  if (length) {
    memcpy(gltang_custom_bytes(custom) + offset, bytes, length);
  }
  return GLTANG_OK;
}

GLTANG_Result gltang_handle_custom_set_error(GLTANG_NativeCall * call, GLTANG_Handle handle, bool is_error) {
  GLTANG_CustomObject * custom = custom_of(call, handle);
  if (!custom) {
    return GLTANG_ERR_INVALID;
  }
  // A flag in the header, never traced: no barrier.
  if (is_error) {
    custom->flags = (uint16_t)(custom->flags | GLTANG_CUSTOM_FLAG_ERROR);
  }
  else {
    custom->flags = (uint16_t)(custom->flags & ~GLTANG_CUSTOM_FLAG_ERROR);
  }
  return GLTANG_OK;
}

bool gltang_handle_custom_is_error(const GLTANG_NativeCall * call, GLTANG_Handle handle) {
  const GLTANG_CustomObject * custom = custom_of(call, handle);
  return custom && (custom->flags & GLTANG_CUSTOM_FLAG_ERROR) != 0;
}

/* For the tests only; declared in vm/test_hooks.h. */
void * gltang_vm_test_handle_object(const GLTANG_NativeCall * call, GLTANG_Handle handle) {
  bool ok;
  GLTANG_Value v = gltang_vm_handle_value(call, handle, &ok);
  return ok && gltang_v_is_object(v) ? gltang_object(v) : NULL;
}

// ---------------------------------------------------------------------------
// The runtime poll
// ---------------------------------------------------------------------------

GLTANG_Result gltang_call_poll(GLTANG_NativeCall * call, uint64_t work) {
  if (!call) {
    return GLTANG_ERR_INVALID;
  }
  if (call->scope_unwind) {
    return GLTANG_ERR_LIMIT;
  }
  // Continue or unwind only (AD-21): the callback's C frame is live here.
  if (GLTANG_NATIVE_POLL(call->exec, HOST_CALLBACK, work ? work : 1u) != GLTANG_ST_OK) {
    call->scope_unwind = true;
    return GLTANG_ERR_LIMIT;
  }
  return GLTANG_OK;
}

// ---------------------------------------------------------------------------
// The answer
// ---------------------------------------------------------------------------

void gltang_call_return_handle(GLTANG_NativeCall * call, GLTANG_Handle handle) {
  if (!call) {
    return;
  }
  gltang_vm_call_forget_answer(call);
  call->set = GLTANG_CALL_HANDLE;
  call->answer_handle = handle;
}

// ---------------------------------------------------------------------------
// Persistent handles
// ---------------------------------------------------------------------------

static bool persistent_room(GLTANG_Execution * exec, GLTANG_HandleState * hs) {
  if (hs->persistent_used < hs->persistent_capacity) {
    return true;
  }
  // The limit is checked by the caller, once, and is not also the size of the table.
  size_t capacity = hs->persistent_capacity ? hs->persistent_capacity * 2u : 16u;
  GLTANG_Value * values = gcu_allocator_realloc(exec->allocator, hs->persistent, capacity * sizeof(GLTANG_Value));
  if (!values) {
    return false;
  }
  hs->persistent = values;
  uint32_t * generations = gcu_allocator_realloc(exec->allocator, hs->persistent_generation, capacity * sizeof(uint32_t));
  if (!generations) {
    return false;
  }
  hs->persistent_generation = generations;
  uint32_t * next = gcu_allocator_realloc(exec->allocator, hs->persistent_next, capacity * sizeof(uint32_t));
  if (!next) {
    return false;
  }
  hs->persistent_next = next;
  for (size_t i = hs->persistent_capacity; i < capacity; ++i) {
    hs->persistent[i] = 0;
    hs->persistent_generation[i] = 0;
    hs->persistent_next[i] = 0;
  }
  hs->persistent_capacity = capacity;
  return true;
}

GLTANG_Result gltang_handle_promote(GLTANG_NativeCall * call, GLTANG_Handle handle, GLTANG_Persistent * out) {
  bool ok;
  GLTANG_Value v = gltang_vm_handle_value(call, handle, &ok);
  if (!ok || !out) {
    return GLTANG_ERR_INVALID;
  }
  GLTANG_Execution * exec = call->exec;
  GLTANG_HandleState * hs = state_of(exec);
  if (!hs) {
    return GLTANG_ERR_LIMIT;
  }
  if (hs->persistent_live >= hs->persistent_limit) {
    return GLTANG_ERR_LIMIT;
  }
  uint32_t slot;
  if (hs->persistent_free) {
    slot = hs->persistent_free - 1u;
    hs->persistent_free = hs->persistent_next[slot];
  }
  else {
    if (!persistent_room(exec, hs)) {
      return GLTANG_ERR_LIMIT;
    }
    slot = (uint32_t)hs->persistent_used++;
  }
  hs->persistent[slot] = v;
  ++hs->persistent_generation[slot];   // odd: in use
  ++hs->persistent_live;
  out->index = slot;
  out->generation = hs->persistent_generation[slot];
  return GLTANG_OK;
}

static bool persistent_ok(const GLTANG_Execution * exec, GLTANG_Persistent p) {
  const GLTANG_HandleState * hs = exec->handles;
  return hs && p.index < hs->persistent_used && (p.generation & 1u) && hs->persistent_generation[p.index] == p.generation;
}

GLTANG_Result gltang_persistent_get(GLTANG_NativeCall * call, GLTANG_Persistent persistent, GLTANG_Handle * out) {
  if (!call || !out || !persistent_ok(call->exec, persistent)) {
    return GLTANG_ERR_INVALID;
  }
  GLTANG_Handle h;
  GLTANG_Result r = push_handle(call, call->exec->handles->persistent[persistent.index], &h);
  if (r == GLTANG_OK) {
    *out = h;
  }
  return r;
}

GLTANG_Result gltang_persistent_release(GLTANG_NativeCall * call, GLTANG_Persistent persistent) {
  if (!call || !persistent_ok(call->exec, persistent)) {
    return GLTANG_ERR_INVALID;
  }
  GLTANG_HandleState * hs = call->exec->handles;
  hs->persistent[persistent.index] = 0;
  ++hs->persistent_generation[persistent.index];   // even: free
  hs->persistent_next[persistent.index] = hs->persistent_free;
  hs->persistent_free = persistent.index + 1u;
  --hs->persistent_live;
  return GLTANG_OK;
}

void gltang_vm_handles_enumerate(GLTANG_Execution * exec, const GRCORE_RootVisitor * visitor) {
  const GLTANG_HandleState * hs = exec->handles;
  if (!hs) {
    return;
  }
  for (size_t i = 0; i < hs->persistent_used; ++i) {
    visitor->slot(visitor->user, &hs->persistent[i]);
  }
}

void gltang_vm_handles_destroy(GLTANG_Execution * exec) {
  GLTANG_HandleState * hs = exec->handles;
  if (!hs) {
    return;   // most executions never used a handle: nothing was made
  }
  gcu_allocator_free(exec->allocator, hs->frames);
  gcu_allocator_free(exec->allocator, hs->persistent);
  gcu_allocator_free(exec->allocator, hs->persistent_generation);
  gcu_allocator_free(exec->allocator, hs->persistent_next);
  for (size_t i = 0; i < hs->held_library_count; ++i) {
    gltang_library_release(hs->held_libraries[i]);
  }
  gcu_allocator_free(exec->allocator, hs->held_libraries);
  for (size_t i = 0; i < hs->owned_member_count; ++i) {
    gcu_allocator_free(exec->allocator, hs->owned_members[i]);
  }
  gcu_allocator_free(exec->allocator, hs->owned_members);
  // Last: an instance's type lives in its registry, and the heap's objects are
  // gone or unreachable by now.
  for (size_t i = 0; i < hs->registry_count; ++i) {
    gltang_type_registry_release(hs->registries[i]);
  }
  gcu_allocator_free(exec->allocator, hs->registries);
  gcu_allocator_free(exec->allocator, hs);
  exec->handles = NULL;
}

GLTANG_Result gltang_execution_set_handle_limits(GLTANG_Execution * execution, size_t scope_handles, size_t persistent_handles) {
  if (!execution || execution->in_host || execution->destroyed) {
    return GLTANG_ERR_INVALID;
  }
  if (scope_handles > SCOPE_LIMIT_MAX || persistent_handles > PERSISTENT_LIMIT_MAX) {
    return GLTANG_ERR_LIMIT;
  }
  GLTANG_HandleState * hs = state_of(execution);
  if (!hs) {
    return GLTANG_ERR_OOM;
  }
  hs->scope_limit = scope_handles ? scope_handles : GLTANG_SCOPE_LIMIT_DEFAULT;
  hs->persistent_limit = persistent_handles ? persistent_handles : GLTANG_PERSISTENT_LIMIT_DEFAULT;
  return GLTANG_OK;
}
