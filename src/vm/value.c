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
 * Values: the heap types, the codec, boxing, kinds and errors.
 */

#include <ghoti.io/lang-tang/macros.h>

#include <string.h>
#include <ghoti.io/lang-tang/value.h>
#include "vm_internal.h"

// ---------------------------------------------------------------------------
// Heap types. The trace functions report every slot that can hold a value or a
// pointer, and nothing else; string and number objects are leaves.
// ---------------------------------------------------------------------------

static void array_trace(GRHEAP_Tracer * tracer, void * payload) {
  GLTANG_ArrayObject * array = payload;
  grheap_trace_slot(tracer, &array->store.raw);
}

// Every slot, used or not: the unused ones are zero, which is null, which the
// codec's tag test passes and the tracer ignores.
static void array_store_trace(GRHEAP_Tracer * tracer, void * payload) {
  GLTANG_ArrayStore * store = payload;
  for (uint64_t i = 0; i < store->capacity; ++i) {
    grheap_trace_word(tracer, &store->slots[i]);
  }
}

static void map_trace(GRHEAP_Tracer * tracer, void * payload) {
  GLTANG_MapObject * map = payload;
  grheap_trace_slot(tracer, &map->store.raw);
}

static void map_store_trace(GRHEAP_Tracer * tracer, void * payload) {
  GLTANG_MapStore * store = payload;
  for (uint64_t i = 0; i < store->capacity * 2u; ++i) {
    grheap_trace_word(tracer, &store->slots[i]);
  }
}

const GRHEAP_Type gltang_type_integer = {"lang-tang integer", sizeof(GLTANG_IntegerObject), NULL, NULL, NULL};
const GRHEAP_Type gltang_type_float = {"lang-tang float", sizeof(GLTANG_FloatObject), NULL, NULL, NULL};
const GRHEAP_Type gltang_type_string = {"lang-tang string", sizeof(GLTANG_StringBlock), NULL, NULL, NULL};
const GRHEAP_Type gltang_type_array = {"lang-tang array", sizeof(GLTANG_ArrayObject), array_trace, NULL, NULL};
const GRHEAP_Type gltang_type_array_store = {"lang-tang array storage", sizeof(GLTANG_ArrayStore), array_store_trace, NULL, NULL};
const GRHEAP_Type gltang_type_map = {"lang-tang map", sizeof(GLTANG_MapObject), map_trace, NULL, NULL};
const GRHEAP_Type gltang_type_map_store = {"lang-tang map storage", sizeof(GLTANG_MapStore), map_store_trace, NULL, NULL};
const GRHEAP_Type gltang_type_error = {"lang-tang error", sizeof(GLTANG_ErrorObject), NULL, NULL, NULL};

// ---------------------------------------------------------------------------
// The codec
// ---------------------------------------------------------------------------

void gltang_value_codec(GRHEAP_ValueCodec * out_codec) {
  if (!out_codec) {
    return;
  }
  // A word is a pointer when its low four bits are clear; it then is its own
  // address. Everything else - integers, booleans, function values - is an
  // immediate, which the tracer ignores without comment.
  out_codec->tag_mask = GLTANG_TAG_MASK;
  out_codec->tag_value = 0;
  out_codec->mask = UINT64_MAX;
  out_codec->shift = 0;
  out_codec->base = 0;
}

GLTANG_Result gltang_heap_options_configure(GRHEAP_Options * options) {
  if (!options) {
    return GLTANG_ERR_INVALID;
  }
  GRHEAP_ValueCodec codec;
  gltang_value_codec(&codec);
  return grheap_options_set_value_codec(options, &codec) == GRHEAP_OK ? GLTANG_OK : GLTANG_ERR_INVALID;
}

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

static const char * const error_messages[GLTANG_ERROR_KIND_COUNT] = {
  [GLTANG_ERROR_DIVIDE_BY_ZERO] = "Divide by zero",
  [GLTANG_ERROR_MODULO_BY_ZERO] = "Modulo by zero",
  [GLTANG_ERROR_NOT_SUPPORTED] = "Not supported",
  [GLTANG_ERROR_NOT_IMPLEMENTED] = "Not implemented",
  [GLTANG_ERROR_INVALID_INDEX] = "Invalid index",
  [GLTANG_ERROR_MAP_KEY_NOT_STRING] = "Map Key Is Not A String",
  [GLTANG_ERROR_INVALID_FUNCTION_CALL] = "Invalid function call",
  [GLTANG_ERROR_ARGUMENT_COUNT_MISMATCH] = "Argument Count Mismatch",
  [GLTANG_ERROR_RECURSION_LIMIT] = "Recursion Limit Exceeded",
  [GLTANG_ERROR_OUT_OF_MEMORY] = "Out of memory",
  [GLTANG_ERROR_INTEGER_TOO_LARGE] = "[INTEGER TOO LARGE]",
  [GLTANG_ERROR_INTEGER_TOO_SMALL] = "[INTEGER TOO SMALL]",
  [GLTANG_ERROR_NOT_A_NUMBER] = "[NOT A NUMBER]",
};

const char * gltang_error_kind_message(GLTANG_ErrorKind kind) {
  if ((unsigned)kind >= (unsigned)GLTANG_ERROR_KIND_COUNT) {
    return "Unknown error";
  }
  return error_messages[kind];
}

bool gltang_error_kind_is_marker(GLTANG_ErrorKind kind) {
  return kind == GLTANG_ERROR_INTEGER_TOO_LARGE || kind == GLTANG_ERROR_INTEGER_TOO_SMALL || kind == GLTANG_ERROR_NOT_A_NUMBER;
}

// ---------------------------------------------------------------------------
// Kinds and accessors
// ---------------------------------------------------------------------------

GLTANG_ValueKind gltang_vm_kind(GLTANG_Value v) {
  if (v == GLTANG_V_NULL) {
    return GLTANG_KIND_NULL;
  }
  if (gltang_v_is_small_int(v)) {
    return GLTANG_KIND_INTEGER;
  }
  if (gltang_v_is_bool(v)) {
    return GLTANG_KIND_BOOL;
  }
  if (gltang_v_is_function(v)) {
    return GLTANG_KIND_FUNCTION;
  }
  if (!gltang_v_is_object(v)) {
    return GLTANG_KIND_NULL;
  }
  switch (gltang_object_kind(v)) {
    case GLTANG_OBJ_INTEGER: return GLTANG_KIND_INTEGER;
    case GLTANG_OBJ_FLOAT: return GLTANG_KIND_FLOAT;
    case GLTANG_OBJ_STRING: return GLTANG_KIND_STRING;
    case GLTANG_OBJ_ARRAY: return GLTANG_KIND_ARRAY;
    case GLTANG_OBJ_MAP: return GLTANG_KIND_MAP;
    case GLTANG_OBJ_ERROR: return GLTANG_KIND_ERROR;
    default: return GLTANG_KIND_NULL;
  }
}

bool gltang_vm_is_error(GLTANG_Value v) {
  return gltang_v_is_kind(v, GLTANG_OBJ_ERROR);
}

bool gltang_vm_is_number(GLTANG_Value v) {
  return gltang_v_is_small_int(v) || gltang_v_is_kind(v, GLTANG_OBJ_INTEGER) || gltang_v_is_kind(v, GLTANG_OBJ_FLOAT);
}

bool gltang_vm_is_container(GLTANG_Value v) {
  return gltang_v_is_kind(v, GLTANG_OBJ_ARRAY) || gltang_v_is_kind(v, GLTANG_OBJ_MAP);
}

int64_t gltang_vm_int(GLTANG_Value v) {
  if (gltang_v_is_small_int(v)) {
    return gltang_v_small_int(v);
  }
  return ((const GLTANG_IntegerObject *)gltang_object(v))->value;
}

double gltang_vm_float(GLTANG_Value v) {
  return ((const GLTANG_FloatObject *)gltang_object(v))->value;
}

double gltang_vm_number(GLTANG_Value v) {
  if (gltang_v_is_kind(v, GLTANG_OBJ_FLOAT)) {
    return gltang_vm_float(v);
  }
  return (double)gltang_vm_int(v);
}

bool gltang_vm_truthy(GLTANG_Value v) {
  switch (gltang_vm_kind(v)) {
    case GLTANG_KIND_NULL: return false;
    case GLTANG_KIND_BOOL: return v == GLTANG_V_TRUE;
    case GLTANG_KIND_INTEGER: return gltang_vm_int(v) != 0;
    case GLTANG_KIND_FLOAT: return gltang_vm_float(v) != 0.0;
    case GLTANG_KIND_STRING: return gltang_vm_string(v)->byte_length != 0;
    case GLTANG_KIND_ARRAY: return gltang_vm_array(v)->length != 0;
    case GLTANG_KIND_MAP: return gltang_vm_map(v)->count != 0;
    case GLTANG_KIND_FUNCTION: return true;  // unspecified by the reference
    case GLTANG_KIND_ERROR: return false;
  }
  return false;
}

GLTANG_Value gltang_vm_failure_value(GLTANG_Execution * exec, GLTANG_Status st) {
  if (st == GLTANG_ST_UNWIND) {
    return GLTANG_V_UNWIND;
  }
  return exec->roots[GLTANG_ROOT_OOM];
}

GLTANG_Value gltang_vm_make_int(GLTANG_Execution * exec, int64_t n) {
  if (n >= GLTANG_SMALL_INT_MIN && n <= GLTANG_SMALL_INT_MAX) {
    return gltang_v_from_small_int(n);
  }
  void * object;
  GLTANG_Status st = gltang_vm_alloc(exec, &gltang_type_integer, sizeof(GLTANG_IntegerObject), &object);
  if (st != GLTANG_ST_OK) {
    return gltang_vm_failure_value(exec, st);
  }
  GLTANG_IntegerObject * box = object;
  box->kind = GLTANG_OBJ_INTEGER;
  box->value = n;
  return gltang_value_of(box);
}

GLTANG_Value gltang_vm_make_float(GLTANG_Execution * exec, double d) {
  void * object;
  GLTANG_Status st = gltang_vm_alloc(exec, &gltang_type_float, sizeof(GLTANG_FloatObject), &object);
  if (st != GLTANG_ST_OK) {
    return gltang_vm_failure_value(exec, st);
  }
  GLTANG_FloatObject * box = object;
  box->kind = GLTANG_OBJ_FLOAT;
  box->value = d;
  return gltang_value_of(box);
}

GLTANG_Value gltang_vm_make_error(GLTANG_Execution * exec, GLTANG_ErrorKind kind) {
  void * object;
  GLTANG_Status st = gltang_vm_alloc(exec, &gltang_type_error, sizeof(GLTANG_ErrorObject), &object);
  if (st != GLTANG_ST_OK) {
    return gltang_vm_failure_value(exec, st);
  }
  GLTANG_ErrorObject * error = object;
  error->kind = GLTANG_OBJ_ERROR;
  error->error_kind = (uint32_t)kind;
  // The origin: the place the failing instruction is, in the terms a poll uses.
  error->function = exec->current_function;
  error->offset = exec->current_offset;
  return gltang_value_of(error);
}

GLTANG_ErrorKind gltang_vm_error_kind(GLTANG_Value v) {
  return (GLTANG_ErrorKind)((const GLTANG_ErrorObject *)gltang_object(v))->error_kind;
}
