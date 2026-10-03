/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2024-2026 Corey Pennycuff
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
 * The execution: the object that ties a program to a context, the engine
 * descriptor runtime-core reads, the root source the collector pulls, and the
 * accessors a host reads results with.
 *
 * Everything the execution allocates comes from the context's counting
 * allocator, so the context's meter is exact (AD-13). It is the context's
 * keyed state (AD-19): destroying the context frees it, after the heap's own
 * destructor has not yet run (reverse registration order, AD-20).
 */

#include <ghoti.io/lang-tang/macros.h>

#include <stdio.h>
#include <string.h>
#include <ghoti.io/cutil/memory.h>
#include "vm_internal.h"

// ---------------------------------------------------------------------------
// Where the execution is
// ---------------------------------------------------------------------------

static GLTANG_Execution * execution_of(const GRCORE_Context * context) {
  return context ? grcore_context_slot(context, &gltang_execution_key) : NULL;
}

void gltang_vm_flush_fuel(GLTANG_Execution * exec) {
  if (exec->pending_fuel) {
    grcore_context_charge_fuel(exec->context, exec->pending_fuel);
    exec->pending_fuel = 0;
  }
}

GRCORE_Location gltang_vm_location(const GLTANG_Execution * exec) {
  GRCORE_Location where = {NULL, 0};
  if (exec->program) {
    where.file = exec->program->file;
    int line = 0;
    if (gltang_program_locate(exec->program, exec->current_function, exec->current_offset, &line) == GLTANG_OK) {
      where.line = line;
    }
  }
  return where;
}

GLTANG_Status gltang_vm_native_poll(GLTANG_Execution * exec, uint64_t work) {
  gltang_vm_flush_fuel(exec);
  GRCORE_Result r = grcore_runtime_poll(exec->context, work, gltang_vm_location(exec));
  if (r != GRCORE_OK) {
    exec->unwinding = true;
    return GLTANG_ST_UNWIND;
  }
  return GLTANG_ST_OK;
}

GLTANG_Status gltang_vm_alloc(GLTANG_Execution * exec, const GRHEAP_Type * type, size_t bytes, void ** out) {
  GRHEAP_Result r = grheap_alloc_sized(exec->heap, type, bytes, out);
  if (r == GRHEAP_OK) {
    return GLTANG_ST_OK;
  }
  if (r == GRHEAP_ERR_LIMIT) {
    // The context's memory budget refused the allocation (AD-21): the reserve
    // is gone. The runtime poll turns that into a verdict, after the collector
    // has had its chance to reclaim; if it still says continue, the operation
    // fails as an allocation failure does.
    if (gltang_vm_native_poll(exec, 0) != GLTANG_ST_OK) {
      return GLTANG_ST_UNWIND;
    }
  }
  return GLTANG_ST_OOM;
}

bool gltang_vm_temp_push(GLTANG_Execution * exec, GLTANG_Value v) {
  if (exec->temp_count == exec->temp_capacity) {
    size_t capacity = exec->temp_capacity ? exec->temp_capacity * 2u : 64u;
    GLTANG_Value * temps = gcu_allocator_realloc(exec->allocator, exec->temps, capacity * sizeof(GLTANG_Value));
    if (!temps) {
      return false;
    }
    exec->temps = temps;
    exec->temp_capacity = capacity;
  }
  exec->temps[exec->temp_count++] = v;
  return true;
}

void gltang_vm_temp_pop(GLTANG_Execution * exec) {
  exec->temps[--exec->temp_count] = 0;
}

GLTANG_Value gltang_vm_constant(GLTANG_Execution * exec, uint32_t index) {
  GLTANG_Value cached = exec->constants[index];
  if (cached) {
    return cached;
  }
  const GLTANG_Const * c = &exec->program->constants[index];
  GLTANG_Value v;
  switch (c->kind) {
    case GLTANG_CONST_INTEGER: v = gltang_vm_make_int(exec, c->integer); break;
    case GLTANG_CONST_FLOAT: v = gltang_vm_make_float(exec, c->number); break;
    default: v = gltang_vm_string_from_block(exec, c->block, c->block_size); break;
  }
  if (v == GLTANG_V_UNWIND || v == exec->roots[GLTANG_ROOT_OOM]) {
    return v;
  }
  exec->constants[index] = v;
  return v;
}

// ---------------------------------------------------------------------------
// Use
// ---------------------------------------------------------------------------

GLTANG_Value gltang_vm_resolve(GLTANG_Execution * exec, const GLTANG_StringBlock * path) {
  // The one place a library is looked up. There is no registry yet: a host
  // may give names through its resolver, and a name nobody provides is null
  // (reference 9.1).
  if (!exec->resolver) {
    return GLTANG_V_NULL;
  }
  GLTANG_HostValue host;
  memset(&host, 0, sizeof(host));
  if (!exec->resolver(exec->resolver_user, gltang_string_bytes(path), &host)) {
    return GLTANG_V_NULL;
  }
  switch (host.kind) {
    case GLTANG_HOST_BOOL: return gltang_v_from_bool(host.boolean);
    case GLTANG_HOST_INTEGER: return gltang_vm_make_int(exec, host.integer);
    case GLTANG_HOST_FLOAT: return gltang_vm_make_float(exec, host.number);
    case GLTANG_HOST_STRING:
      if (!host.text) {
        return GLTANG_V_NULL;
      }
      return gltang_vm_string_from_utf8(exec, host.text, host.length, host.encoding);
    default:
      return GLTANG_V_NULL;
  }
}

// ---------------------------------------------------------------------------
// The engine descriptor (AD-18)
// ---------------------------------------------------------------------------

static GRCORE_SlotKind descriptor_slot_kind(const GRCORE_AbstractFrame * frame, size_t index) {
  (void)frame;
  // The header words (function, pc, sp, flags) are plain bits. Everything
  // after is a local or an operand-stack slot, which holds a value.
  return index < GLTANG_FRAME_HEADER ? GRCORE_SLOT_RAW : GRCORE_SLOT_VALUE;
}

static GRCORE_Location descriptor_locate(const GRCORE_Context * context, uint64_t function, uint64_t offset) {
  GRCORE_Location where = {NULL, 0};
  GLTANG_Execution * exec = execution_of(context);
  if (!exec || exec->destroyed || !exec->program) {
    return where;
  }
  where.file = exec->program->file;
  int line = 0;
  if (gltang_program_locate(exec->program, function, offset, &line) == GLTANG_OK) {
    where.line = line;
  }
  return where;
}

static size_t descriptor_inspect(const GRCORE_Context * context, GRCORE_SlotKind kind, uint64_t value, char * buffer, size_t size) {
  if (kind == GRCORE_SLOT_RAW) {
    int n = snprintf(buffer, size, "%llu", (unsigned long long)value);
    return n < 0 ? 0 : (size_t)n;
  }
  GLTANG_Execution * exec = execution_of(context);
  if (!exec || exec->destroyed) {
    // Without a live execution there is no heap to read the value from.
    int n = snprintf(buffer, size, "%llu", (unsigned long long)value);
    return n < 0 ? 0 : (size_t)n;
  }
  GLTANG_Sink sink;
  gltang_sink_init_buffer(&sink, buffer, size);
  sink.exec = exec;
  sink.limit = 1024;
  bool too_deep = false;
  // Printed as an element would be, a few levels deep: a debugger shows a
  // summary, not a megabyte.
  (void)gltang_vm_render(&sink, value, GLTANG_RENDER_ELEMENT, GLTANG_MAX_VALUE_DEPTH - 3, &too_deep);
  return sink.needed;
}

static GLTANG_Execution * frame_execution(const GRCORE_AbstractFrame * frame) {
  GLTANG_Execution * exec = execution_of(frame->context);
  return exec && !exec->destroyed ? exec : NULL;
}

static bool frame_function(const GRCORE_AbstractFrame * frame, uint64_t * function) {
  GRCORE_Stack * stack = grcore_context_stack(frame->context);
  return stack && grcore_stack_slot_get(stack, frame->frame, GLTANG_F_FUNCTION, function) == GRCORE_OK;
}

static size_t descriptor_scope_count(const GRCORE_AbstractFrame * frame) {
  GLTANG_Execution * exec = frame_execution(frame);
  uint64_t function;
  if (!exec || !frame_function(frame, &function) || function >= exec->program->function_count) {
    return 0;
  }
  return function == 0 ? 1u : 2u;
}

static size_t named_locals(const GLTANG_Function * f) {
  size_t n = 0;
  for (uint32_t i = 0; i < f->local_count; ++i) {
    n += f->local_names[i] != NULL;
  }
  return n;
}

static GRCORE_Result descriptor_scope(const GRCORE_AbstractFrame * frame, size_t index, GRCORE_ScopeInfo * out) {
  GLTANG_Execution * exec = frame_execution(frame);
  uint64_t function;
  if (!exec || !frame_function(frame, &function) || function >= exec->program->function_count) {
    return GRCORE_ERR_INVALID;
  }
  const GLTANG_Function * f = &exec->program->functions[function];
  if (function != 0 && index == 0) {
    *out = (GRCORE_ScopeInfo){GRCORE_SCOPE_LOCAL, f->name, named_locals(f)};
    return GRCORE_OK;
  }
  if (index == (function == 0 ? 0u : 1u)) {
    *out = (GRCORE_ScopeInfo){GRCORE_SCOPE_GLOBAL, "program", exec->program->global_count};
    return GRCORE_OK;
  }
  return GRCORE_ERR_INVALID;
}

static GRCORE_Result descriptor_variable(const GRCORE_AbstractFrame * frame, size_t scope, size_t index, GRCORE_Variable * out) {
  GLTANG_Execution * exec = frame_execution(frame);
  uint64_t function;
  if (!exec || !frame_function(frame, &function) || function >= exec->program->function_count) {
    return GRCORE_ERR_INVALID;
  }
  const GLTANG_Function * f = &exec->program->functions[function];
  if (function != 0 && scope == 0) {
    size_t seen = 0;
    for (uint32_t i = 0; i < f->local_count; ++i) {
      if (!f->local_names[i]) {
        continue;
      }
      if (seen++ == index) {
        uint64_t value;
        GRCORE_Stack * stack = grcore_context_stack(frame->context);
        if (grcore_stack_slot_get(stack, frame->frame, GLTANG_FRAME_HEADER + i, &value) != GRCORE_OK) {
          return GRCORE_ERR_INVALID;
        }
        *out = (GRCORE_Variable){f->local_names[i], GRCORE_SLOT_VALUE, value};
        return GRCORE_OK;
      }
    }
    return GRCORE_ERR_INVALID;
  }
  if (scope == (function == 0 ? 0u : 1u) && index < exec->program->global_count) {
    *out = (GRCORE_Variable){exec->program->global_names[index], GRCORE_SLOT_VALUE, exec->globals[index]};
    return GRCORE_OK;
  }
  return GRCORE_ERR_INVALID;
}

static void descriptor_unwind(GRCORE_Context * context, const GRCORE_AbstractFrame * frame) {
  (void)frame;
  // A frame holds only values, which the collector owns, so there is nothing
  // to release; the hook counts the frames the unwinder pops.
  GLTANG_Execution * exec = execution_of(context);
  if (exec) {
    ++exec->frames_unwound;
  }
}

const GRCORE_EngineDescriptor gltang_engine_descriptor = {
  .name = "lang-tang",
  .slot_kind = descriptor_slot_kind,
  .locate = descriptor_locate,
  .inspect = descriptor_inspect,
  .scopes = {descriptor_scope_count, descriptor_scope, descriptor_variable},
  .decoder = {UINT64_MAX, 0, 0},
  .roots = NULL,
  .unwind = descriptor_unwind,
};

// ---------------------------------------------------------------------------
// Roots
// ---------------------------------------------------------------------------

static void enumerate_roots(GRCORE_Context * context, void * value, const GRCORE_RootVisitor * visitor) {
  (void)context;
  GLTANG_Execution * exec = value;
  if (!visitor->slot) {
    return;
  }
  for (size_t i = 0; i < exec->root_count; ++i) {
    visitor->slot(visitor->user, &exec->roots[i]);
  }
  for (size_t i = 0; i < exec->temp_count; ++i) {
    visitor->slot(visitor->user, &exec->temps[i]);
  }
}

static const GRCORE_RootSource root_source = {"lang-tang execution", enumerate_roots};

// ---------------------------------------------------------------------------
// Create and destroy
// ---------------------------------------------------------------------------

static void release_parts(GLTANG_Execution * exec) {
  if (exec->destroyed) {
    return;
  }
  exec->destroyed = true;
  grcore_context_remove_root_source(exec->context, &root_source, exec);
  gcu_allocator_free(exec->allocator, exec->roots);
  gcu_allocator_free(exec->allocator, exec->temps);
  gcu_allocator_free(exec->allocator, exec->output);
  gcu_allocator_free(exec->allocator, exec->segments);
  exec->roots = exec->globals = exec->constants = exec->temps = NULL;
  exec->output = NULL;
  exec->segments = NULL;
  exec->root_count = exec->temp_count = 0;
  exec->output_length = exec->segment_count = 0;
  gltang_program_release(exec->program);
  exec->program = NULL;
}

static void key_destroy(GRCORE_Context * context, void * value) {
  (void)context;
  GLTANG_Execution * exec = value;
  release_parts(exec);
  gcu_allocator_free(exec->allocator, exec);
}

const GRCORE_Key gltang_execution_key = {
  .name = "lang-tang execution",
  .cardinality = GRCORE_CARDINALITY_ONE,
  .phase = GRCORE_PHASE_NONE,
  .destroy = key_destroy,
  .poll = NULL,
};

static GLTANG_Result map_core_result(GRCORE_Result r) {
  switch (r) {
    case GRCORE_OK: return GLTANG_OK;
    case GRCORE_ERR_LIMIT: return GLTANG_ERR_LIMIT;
    case GRCORE_ERR_OOM: return GLTANG_ERR_OOM;
    default: return GLTANG_ERR_INVALID;
  }
}

GLTANG_Result gltang_execution_create(GRCORE_Context * context, GLTANG_Program * program, GLTANG_Execution ** out_execution) {
  if (!context || !program || !out_execution || !grcore_context_is_owner(context)) {
    return GLTANG_ERR_INVALID;
  }
  GRHEAP_Heap * heap = grheap_heap_get(context);
  if (!heap || grcore_context_slot(context, &gltang_execution_key)) {
    return GLTANG_ERR_INVALID;
  }
  const GRCORE_Allocator * allocator = grcore_context_allocator(context);
  GLTANG_Execution * exec = gcu_allocator_calloc(allocator, 1, sizeof(GLTANG_Execution));
  if (!exec) {
    return GLTANG_ERR_OOM;
  }
  exec->context = context;
  exec->heap = heap;
  exec->allocator = allocator;
  exec->root_count = GLTANG_ROOT_FIXED + program->global_count + program->constant_count;
  exec->roots = gcu_allocator_calloc(allocator, exec->root_count, sizeof(GLTANG_Value));
  if (!exec->roots) {
    gcu_allocator_free(allocator, exec);
    return GLTANG_ERR_OOM;
  }
  exec->globals = exec->roots + GLTANG_ROOT_FIXED;
  exec->constants = exec->globals + program->global_count;
  exec->program = gltang_program_retain(program);

  // The engine is registered once per context. A create that failed after
  // this point leaves it registered, so a second attempt finds it again.
  GRCORE_EngineId engine = 0;
  for (GRCORE_EngineId id = 1; id <= grcore_engine_count(context); ++id) {
    if (grcore_engine_descriptor(context, id) == &gltang_engine_descriptor) {
      engine = id;
    }
  }
  GLTANG_Result result = GLTANG_OK;
  if (!engine) {
    result = map_core_result(grcore_engine_register(context, &gltang_engine_descriptor, &engine));
  }
  bool root_added = false;
  if (result == GLTANG_OK) {
    exec->engine = engine;
    result = map_core_result(grcore_context_add_root_source(context, &root_source, exec));
    root_added = result == GLTANG_OK;
  }
  if (result == GLTANG_OK) {
    // The Out of memory value is made now, once, so that running out of
    // memory never has to allocate to say so.
    void * object;
    GLTANG_Status st = gltang_vm_alloc(exec, &gltang_type_error, sizeof(GLTANG_ErrorObject), &object);
    if (st != GLTANG_ST_OK) {
      result = st == GLTANG_ST_UNWIND ? GLTANG_ERR_LIMIT : GLTANG_ERR_OOM;
    }
    else {
      GLTANG_ErrorObject * error = object;
      error->kind = GLTANG_OBJ_ERROR;
      error->error_kind = GLTANG_ERROR_OUT_OF_MEMORY;
      exec->roots[GLTANG_ROOT_OOM] = gltang_value_of(error);
    }
  }
  if (result == GLTANG_OK) {
    result = map_core_result(grcore_context_register(context, &gltang_execution_key, exec));
  }
  if (result != GLTANG_OK) {
    if (root_added) {
      grcore_context_remove_root_source(context, &root_source, exec);
    }
    exec->destroyed = true;  // the root source is already gone
    gcu_allocator_free(allocator, exec->roots);
    gltang_program_release(exec->program);
    gcu_allocator_free(allocator, exec);
    return result;
  }
  *out_execution = exec;
  return GLTANG_OK;
}

void gltang_execution_destroy(GLTANG_Execution * execution) {
  if (execution) {
    release_parts(execution);
  }
}

GLTANG_Result gltang_execution_set_resolver(GLTANG_Execution * execution, GLTANG_Resolver resolver, void * user) {
  if (!execution) {
    return GLTANG_ERR_INVALID;
  }
  execution->resolver = resolver;
  execution->resolver_user = user;
  return GLTANG_OK;
}

GLTANG_ExecutionState gltang_execution_state(const GLTANG_Execution * execution) {
  return execution ? execution->state : GLTANG_EXECUTION_NEW;
}

uint64_t gltang_execution_unwound_frames(const GLTANG_Execution * execution) {
  return execution ? execution->frames_unwound : 0;
}

// ---------------------------------------------------------------------------
// The result and the output
// ---------------------------------------------------------------------------

static GLTANG_Value result_of(const GLTANG_Execution * execution) {
  if (!execution || execution->destroyed || !execution->roots) {
    return GLTANG_V_NULL;
  }
  return execution->roots[GLTANG_ROOT_RESULT];
}

GLTANG_ValueKind gltang_execution_result_kind(const GLTANG_Execution * execution) {
  return gltang_vm_kind(result_of(execution));
}

bool gltang_execution_result_bool(const GLTANG_Execution * execution) {
  return result_of(execution) == GLTANG_V_TRUE;
}

int64_t gltang_execution_result_integer(const GLTANG_Execution * execution) {
  GLTANG_Value v = result_of(execution);
  return gltang_vm_kind(v) == GLTANG_KIND_INTEGER ? gltang_vm_int(v) : 0;
}

double gltang_execution_result_float(const GLTANG_Execution * execution) {
  GLTANG_Value v = result_of(execution);
  return gltang_vm_kind(v) == GLTANG_KIND_FLOAT ? gltang_vm_float(v) : 0.0;
}

const char * gltang_execution_result_text(const GLTANG_Execution * execution, size_t * out_length) {
  GLTANG_Value v = result_of(execution);
  if (gltang_v_is_kind(v, GLTANG_OBJ_STRING)) {
    if (out_length) {
      *out_length = (size_t)gltang_vm_string(v)->byte_length;
    }
    return gltang_string_bytes(gltang_vm_string(v));
  }
  if (gltang_vm_is_error(v)) {
    const char * message = gltang_error_kind_message(gltang_vm_error_kind(v));
    if (out_length) {
      *out_length = strlen(message);
    }
    return message;
  }
  return NULL;
}

size_t gltang_execution_result_size(const GLTANG_Execution * execution) {
  GLTANG_Value v = result_of(execution);
  switch (gltang_vm_kind(v)) {
    case GLTANG_KIND_ARRAY: return (size_t)gltang_vm_array(v)->length;
    case GLTANG_KIND_MAP: return (size_t)gltang_vm_map(v)->count;
    case GLTANG_KIND_STRING: return (size_t)gltang_vm_string(v)->grapheme_length;
    default: return 0;
  }
}

GLTANG_Result gltang_execution_result_describe(const GLTANG_Execution * execution, char ** out_text, size_t * out_length) {
  if (!execution || !out_text) {
    return GLTANG_ERR_INVALID;
  }
  GLTANG_Value v = result_of(execution);
  GLTANG_Sink sink;
  // Two passes: the first measures, the second fills.
  gltang_sink_init_buffer(&sink, NULL, 0);
  sink.exec = (GLTANG_Execution *)(uintptr_t)execution;
  bool too_deep = false;
  (void)gltang_vm_render(&sink, v, GLTANG_RENDER_ELEMENT, 0, &too_deep);
  size_t needed = sink.needed;
  char * buffer = gcu_malloc(needed + 1u);
  if (!buffer) {
    return GLTANG_ERR_OOM;
  }
  gltang_sink_init_buffer(&sink, buffer, needed + 1u);
  sink.exec = (GLTANG_Execution *)(uintptr_t)execution;
  too_deep = false;
  (void)gltang_vm_render(&sink, v, GLTANG_RENDER_ELEMENT, 0, &too_deep);
  *out_text = buffer;
  if (out_length) {
    *out_length = needed;
  }
  return GLTANG_OK;
}

bool gltang_execution_result_error(const GLTANG_Execution * execution, GLTANG_ErrorKind * out_kind, GLTANG_ErrorOrigin * out_origin) {
  GLTANG_Value v = result_of(execution);
  if (!gltang_vm_is_error(v)) {
    return false;
  }
  const GLTANG_ErrorObject * error = gltang_object(v);
  if (out_kind) {
    *out_kind = (GLTANG_ErrorKind)error->error_kind;
  }
  if (out_origin) {
    out_origin->file = execution->program ? execution->program->file : NULL;
    out_origin->function = error->function;
    out_origin->offset = error->offset;
    int line = 0;
    if (execution->program && gltang_program_locate(execution->program, error->function, error->offset, &line) != GLTANG_OK) {
      line = 0;
    }
    out_origin->line = line;
  }
  return true;
}

static void fill_item(GLTANG_Value v, GLTANG_ResultItem * out) {
  memset(out, 0, sizeof(*out));
  out->kind = gltang_vm_kind(v);
  out->boolean = v == GLTANG_V_TRUE;
  switch (out->kind) {
    case GLTANG_KIND_INTEGER: out->integer = gltang_vm_int(v); break;
    case GLTANG_KIND_FLOAT: out->number = gltang_vm_float(v); break;
    case GLTANG_KIND_STRING:
      out->text = gltang_string_bytes(gltang_vm_string(v));
      out->length = (size_t)gltang_vm_string(v)->byte_length;
      out->size = (size_t)gltang_vm_string(v)->grapheme_length;
      break;
    case GLTANG_KIND_ARRAY: out->size = (size_t)gltang_vm_array(v)->length; break;
    case GLTANG_KIND_MAP: out->size = (size_t)gltang_vm_map(v)->count; break;
    case GLTANG_KIND_ERROR:
      out->error = gltang_vm_error_kind(v);
      out->text = gltang_error_kind_message(out->error);
      out->length = strlen(out->text);
      break;
    default: break;
  }
}

bool gltang_execution_result_element(const GLTANG_Execution * execution, size_t index, GLTANG_ResultItem * out_item) {
  GLTANG_Value v = result_of(execution);
  if (!out_item || !gltang_v_is_kind(v, GLTANG_OBJ_ARRAY) || index >= gltang_vm_array(v)->length) {
    return false;
  }
  fill_item(gltang_vm_array(v)->store.typed->slots[index], out_item);
  return true;
}

bool gltang_execution_result_member(const GLTANG_Execution * execution, const char * key, GLTANG_ResultItem * out_item) {
  GLTANG_Value v = result_of(execution);
  if (!out_item || !key || !gltang_v_is_kind(v, GLTANG_OBJ_MAP)) {
    return false;
  }
  bool found;
  GLTANG_Value member = gltang_vm_map_get(v, key, strlen(key), &found);
  if (!found) {
    return false;
  }
  fill_item(member, out_item);
  return true;
}

const char * gltang_execution_output_raw(const GLTANG_Execution * execution, size_t * out_length) {
  if (!execution || execution->destroyed || !execution->output) {
    if (out_length) {
      *out_length = 0;
    }
    return "";
  }
  if (out_length) {
    *out_length = execution->output_length;
  }
  return execution->output;
}

GLTANG_Result gltang_execution_output_render(const GLTANG_Execution * execution, char ** out_text, size_t * out_length) {
  if (!execution || !out_text) {
    return GLTANG_ERR_INVALID;
  }
  char * rendered;
  size_t length;
  if (execution->destroyed || !execution->output) {
    rendered = gcu_malloc(1);
    if (!rendered) {
      return GLTANG_ERR_OOM;
    }
    rendered[0] = '\0';
    length = 0;
  }
  else if (!gltang_vm_render_segments(execution->output, execution->output_length, execution->segments, execution->segment_count, &rendered, &length)) {
    return GLTANG_ERR_OOM;
  }
  *out_text = rendered;
  if (out_length) {
    *out_length = length;
  }
  return GLTANG_OK;
}

void gltang_buffer_free(void * buffer) {
  gcu_free(buffer);
}
