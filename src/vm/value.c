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

// An instance of a host-defined type: the slots, as many as its header says
// (AD-11: the engine traces, no module does). The raw bytes after them are
// never traced. The type pointer is the one host pointer it holds, so an
// image refuses it by name (custom.c) until story 8 gives types their hooks.
// A weak slot is traced like any other: it holds the slot's cell, which the
// instance keeps alive, and the cell's target is the heap's to clear.
static void custom_trace(GRHEAP_Tracer * tracer, void * payload) {
  GLTANG_CustomObject * object = payload;
  for (uint32_t i = 0; i < object->slot_count; ++i) {
    grheap_trace_word(tracer, &object->slots[i]);
  }
}

const GRHEAP_Type gltang_type_custom = GRHEAP_TYPE_INIT("lang-tang custom", sizeof(GLTANG_CustomObject), custom_trace, NULL, NULL, gltang_vm_custom_snapshot, NULL);

// An instance of a type with a finalizer: the same layout, trace and image
// hook, and a destroy hook that runs the finalizer (custom.c). A type of its
// own, chosen when the instance is made, so an instance of a type with none
// has no destroy hook to run.
const GRHEAP_Type gltang_type_custom_final = GRHEAP_TYPE_INIT("lang-tang custom final", sizeof(GLTANG_CustomObject), custom_trace, gltang_vm_custom_destroy, NULL, gltang_vm_custom_snapshot, NULL);

// The cell of a weak slot (vm_internal.h, GLTANG_WeakCell): no trace (its one
// slot is the weak one, which the heap handles), the value codec's words
// (`encoded`, so an immediate is left alone and never cleared), and no
// callback when it clears: no host code can run there. An image cannot hold
// a type with a weak spec, so its snapshot hook would never run (snapshot.c
// names the refusal).
static const GRHEAP_WeakSpec weak_cell_spec = {GRHEAP_WEAK_CELL, offsetof(GLTANG_WeakCell, target), 0, 0, true, NULL};
_Static_assert(offsetof(GLTANG_WeakCell, target) % 8u == 0, "the heap reads a weak slot at an 8-byte-aligned offset");

const GRHEAP_Type gltang_type_weak_cell = GRHEAP_TYPE_INIT("lang-tang weak cell", sizeof(GLTANG_WeakCell), NULL, NULL, &weak_cell_spec, NULL, NULL);

const GRHEAP_Type gltang_type_integer = GRHEAP_TYPE_INIT("lang-tang integer", sizeof(GLTANG_IntegerObject), NULL, NULL, NULL, NULL, NULL);
const GRHEAP_Type gltang_type_float = GRHEAP_TYPE_INIT("lang-tang float", sizeof(GLTANG_FloatObject), NULL, NULL, NULL, NULL, NULL);
const GRHEAP_Type gltang_type_string = GRHEAP_TYPE_INIT("lang-tang string", sizeof(GLTANG_StringBlock), NULL, NULL, NULL, NULL, NULL);
const GRHEAP_Type gltang_type_array = GRHEAP_TYPE_INIT("lang-tang array", sizeof(GLTANG_ArrayObject), array_trace, NULL, NULL, NULL, NULL);
const GRHEAP_Type gltang_type_array_store = GRHEAP_TYPE_INIT("lang-tang array storage", sizeof(GLTANG_ArrayStore), array_store_trace, NULL, NULL, NULL, NULL);
const GRHEAP_Type gltang_type_map = GRHEAP_TYPE_INIT("lang-tang map", sizeof(GLTANG_MapObject), map_trace, NULL, NULL, NULL, NULL);
const GRHEAP_Type gltang_type_map_store = GRHEAP_TYPE_INIT("lang-tang map storage", sizeof(GLTANG_MapStore), map_store_trace, NULL, NULL, NULL, NULL);
const GRHEAP_Type gltang_type_error = GRHEAP_TYPE_INIT("lang-tang error", sizeof(GLTANG_ErrorObject), NULL, NULL, NULL, NULL, NULL);

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
  [GLTANG_ERROR_LIMIT_EXCEEDED] = "Limit Exceeded",
  [GLTANG_ERROR_GLOBAL_SEED] = "Cannot change the seed of the global random number generator",
  [GLTANG_ERROR_HOST_FAILED] = "Host function failed",
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
  // A table, not a switch: the custom kind made the switch's code large enough
  // that the compiler stopped inlining this function into its callers in this
  // file (gltang_vm_truthy), which every truth test paid for.
  static const uint8_t kinds[] = {
    [GLTANG_OBJ_INTEGER] = GLTANG_KIND_INTEGER,
    [GLTANG_OBJ_FLOAT] = GLTANG_KIND_FLOAT,
    [GLTANG_OBJ_STRING] = GLTANG_KIND_STRING,
    [GLTANG_OBJ_ARRAY] = GLTANG_KIND_ARRAY,
    [GLTANG_OBJ_ARRAY_STORE] = GLTANG_KIND_NULL,
    [GLTANG_OBJ_MAP] = GLTANG_KIND_MAP,
    [GLTANG_OBJ_MAP_STORE] = GLTANG_KIND_NULL,
    [GLTANG_OBJ_ERROR] = GLTANG_KIND_ERROR,
    [GLTANG_OBJ_LIBRARY] = GLTANG_KIND_LIBRARY,
    [GLTANG_OBJ_NATIVE] = GLTANG_KIND_FUNCTION,
    [GLTANG_OBJ_TEMPLATE] = GLTANG_KIND_FUNCTION,
    [GLTANG_OBJ_RNG] = GLTANG_KIND_RNG,
    [GLTANG_OBJ_CUSTOM] = GLTANG_KIND_CUSTOM,
    // Never a language value; null if one is ever asked about.
    [GLTANG_OBJ_PENDING] = GLTANG_KIND_NULL,
    [GLTANG_OBJ_WEAK] = GLTANG_KIND_NULL,
  };
  uint32_t k = gltang_object_kind(v);
  return k < sizeof kinds ? (GLTANG_ValueKind)kinds[k] : GLTANG_KIND_NULL;
}

/*
 * Which predicate each caller of an error test uses (spec-tang-host-extensibility story 6). An
 * instance marked as standing for an error (handle.h) is not an engine error: no kind, no message,
 * no origin, never listed, no hook. So:
 *
 *   gltang_vm_is_error (engine errors only), because the caller reads an error object or logs it:
 *     errorlist.c gltang_vm_error_swallowed  - reads the object's flags and kind, lists it
 *     errorlist.c gltang_vm_set_result       - reads the object's flags: a result that is lost is listed
 *     execution.c result_text / result_error - read the kind, the message and the origin
 *     interp.c's DISCARD, jit/helpers.c gltang_jit_discard - swallow: a discarded value is logged,
 *                                              and a marked instance is never logged (both tiers agree)
 *     ops.c append_copies, gltang_vm_op_slice, map_store, gltang_vm_op_set_index - a deep copy or a stored value that FAILED (an engine
 *                                              error made by the engine, or the out-of-memory value);
 *                                              an instance of a reference type is stored by reference,
 *                                              and the copy of one of a value type that failed is an
 *                                              engine error too (its `copy` answered one, or the engine
 *                                              made `Host function failed`)
 *     custom.c gltang_vm_custom_iterator_begin and gltang_vm_custom_cast - the error a callback ANSWERED
 *                                              (made by the engine from its status): engine errors only
 *     interp.c's truth sites (JMP_FALSE, JMP_TRUE, OR) - an error a `truthy` answered is recorded with
 *                                              gltang_vm_error_swallowed, which is engine-only
 *   gltang_vm_stands_for_error (engine errors and marked instances), because the caller propagates
 *   the value as the operation's own:
 *     custom.c gltang_vm_custom_binary and the `!=` fallback - an operand or an `equal` answer that
 *                                              stands for an error is the value
 *     container.c equal_rec                  - an element's `equal` answering one is the comparison's value
 *   gltang_vm_is_error, kept, in one more place:
 *     ops.c gltang_vm_op_binary (a string's `+` with an error operand) - a custom operand (a marked instance too) is
 *                                              dispatched to custom_binary on the line before, which
 *                                              uses the predicate above, so the engine-only test is
 *                                              reached only by an engine error
 *   Assignment, a function's result, a statement's value and the `for` loop's value carry the value
 *   unexamined, so a marked instance contaminates them with no test at all.
 */
bool gltang_vm_is_error(GLTANG_Value v) {
  return gltang_v_is_kind(v, GLTANG_OBJ_ERROR);
}

bool gltang_vm_is_number(GLTANG_Value v) {
  return gltang_v_is_number(v);
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
    case GLTANG_KIND_LIBRARY: return true;
    case GLTANG_KIND_RNG: return true;
    case GLTANG_KIND_CUSTOM: return (gltang_vm_custom(v)->flags & GLTANG_CUSTOM_FLAG_ERROR) == 0;  // pure: a marked instance is falsy; the type's `truthy` is asked by gltang_vm_custom_truth, a GC point
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
  error->program = exec->act->program_index;
  error->function = exec->current_function;
  error->offset = exec->current_offset;
  // The host may want it logged now, or the run to end here.
  return gltang_vm_error_created(exec, gltang_value_of(error));
}

GLTANG_ErrorKind gltang_vm_error_kind(GLTANG_Value v) {
  return (GLTANG_ErrorKind)((const GLTANG_ErrorObject *)gltang_object(v))->error_kind;
}
