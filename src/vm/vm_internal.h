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
 * The inside of the interpreter: the value encoding, the heap objects, the
 * execution, and the operations the dispatch loop calls.
 *
 * Nothing here is public. The encoding and the layouts are documented in
 * documentation/design.md, with the alternatives that were rejected.
 *
 * Rules every function in the vm directory follows:
 *
 *  - A value is one 64-bit word (::GLTANG_Value). Never keep a raw pointer to
 *    a heap object in a C variable across a call that can allocate unless the
 *    object is reachable from a frame slot, an execution root or a temporary
 *    root. The collector does not move objects, so the word stays valid while
 *    the object is reachable.
 *  - A pointer stored into a heap object goes through grheap_store*; so does
 *    every other write to a slot the object's trace function reports,
 *    integers included, because barrier-verify compares the slot with the last
 *    value written through the barrier.
 *  - An operation returns a ::GLTANG_Value. The sentinel ::GLTANG_V_UNWIND
 *    means a runtime poll decided the run must stop; everything else,
 *    including every error, is a value and the program goes on.
 */

#ifndef GHOTI_IO_GLTANG_VM_VM_INTERNAL_H
#define GHOTI_IO_GLTANG_VM_VM_INTERNAL_H

#include <ghoti.io/lang-tang/macros.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <ghoti.io/lang-tang/ast/astNodeCast.h>
#include <ghoti.io/lang-tang/bytecode.h>
#include <ghoti.io/lang-tang/execution.h>
#include <ghoti.io/lang-tang/value.h>
#include <ghoti.io/runtime-core/a/engine.h>
#include <ghoti.io/runtime-core/a/frame.h>
#include <ghoti.io/runtime-core/a/stack.h>
#include <ghoti.io/runtime-core/a/unwind.h>
#include <ghoti.io/runtime-core/b/budget.h>
#include <ghoti.io/runtime-core/b/context.h>
#include <ghoti.io/runtime-core/b/poll.h>
#include <ghoti.io/runtime-core/b/roots.h>
#include <ghoti.io/runtime-core/b/run.h>
#include <ghoti.io/runtime-heap/heap.h>
#include <ghoti.io/runtime-heap/roots.h>
#include <ghoti.io/runtime-heap/store.h>
#include <ghoti.io/runtime-heap/type.h>
#include "../compile/program_internal.h"
#include "string_layout.h"

/** @brief One output segment: bytes from `offset` carry `type`. */
typedef struct GLTANG_OutputSegment {
  size_t offset;
  GLTANG_String_Type type;
} GLTANG_OutputSegment;


// ---------------------------------------------------------------------------
// The value encoding
// ---------------------------------------------------------------------------

/** @brief A value: one 64-bit word. */
typedef uint64_t GLTANG_Value;

/*
 * The low four bits are the tag. Heap objects are 16-byte aligned, so a
 * pointer has all four clear and is its own word, and zero is both the null
 * pointer and the null value: a frame slot, a heap payload and a map entry
 * start out as null.
 *
 *   ....0000  pointer to a heap payload, or null when the word is 0
 *   nnnn0001  integer, 60 bits signed; the rest are boxed
 *   ...b0010  boolean: 0x02 false, 0x12 true
 *   iiii0011  function value: the function index in the upper bits
 *   ....1111  never a value: GLTANG_V_UNWIND
 */
#define GLTANG_V_NULL ((GLTANG_Value)0)
#define GLTANG_V_FALSE ((GLTANG_Value)0x02)
#define GLTANG_V_TRUE ((GLTANG_Value)0x12)
#define GLTANG_V_UNWIND ((GLTANG_Value)0x0F)

#define GLTANG_TAG_MASK ((GLTANG_Value)0x0F)
#define GLTANG_TAG_INTEGER ((GLTANG_Value)0x01)
#define GLTANG_TAG_BOOL ((GLTANG_Value)0x02)
#define GLTANG_TAG_FUNCTION ((GLTANG_Value)0x03)

#define GLTANG_SMALL_INT_MAX ((int64_t)(((uint64_t)1 << 59) - 1))
#define GLTANG_SMALL_INT_MIN (-GLTANG_SMALL_INT_MAX - 1)

static inline bool gltang_v_is_object(GLTANG_Value v) {
  return v != 0 && (v & GLTANG_TAG_MASK) == 0;
}
static inline bool gltang_v_is_small_int(GLTANG_Value v) {
  return (v & GLTANG_TAG_MASK) == GLTANG_TAG_INTEGER;
}
static inline bool gltang_v_is_bool(GLTANG_Value v) {
  return (v & GLTANG_TAG_MASK) == GLTANG_TAG_BOOL;
}
static inline bool gltang_v_is_function(GLTANG_Value v) {
  return (v & GLTANG_TAG_MASK) == GLTANG_TAG_FUNCTION;
}
static inline GLTANG_Value gltang_v_from_bool(bool b) {
  return b ? GLTANG_V_TRUE : GLTANG_V_FALSE;
}
static inline GLTANG_Value gltang_v_from_small_int(int64_t n) {
  return ((GLTANG_Value)n << 4) | GLTANG_TAG_INTEGER;
}
static inline int64_t gltang_v_small_int(GLTANG_Value v) {
  return (int64_t)v >> 4;
}
static inline GLTANG_Value gltang_v_from_function(uint64_t index) {
  return (index << 4) | GLTANG_TAG_FUNCTION;
}
static inline uint64_t gltang_v_function_index(GLTANG_Value v) {
  return v >> 4;
}

// ---------------------------------------------------------------------------
// Heap objects. Every payload starts with its kind.
// ---------------------------------------------------------------------------

typedef enum {
  GLTANG_OBJ_INTEGER = 1,   ///< An integer that does not fit the inline form.
  GLTANG_OBJ_FLOAT,         ///< A double.
  GLTANG_OBJ_STRING,        ///< A GLTANG_StringBlock.
  GLTANG_OBJ_ARRAY,         ///< The header of an array.
  GLTANG_OBJ_ARRAY_STORE,   ///< An array's element storage.
  GLTANG_OBJ_MAP,           ///< The header of a map.
  GLTANG_OBJ_MAP_STORE,     ///< A map's entries and index.
  GLTANG_OBJ_ERROR          ///< An error value.
} GLTANG_ObjectKind;

typedef struct GLTANG_IntegerObject {
  uint32_t kind;
  uint32_t reserved;
  int64_t value;
} GLTANG_IntegerObject;

typedef struct GLTANG_FloatObject {
  uint32_t kind;
  uint32_t reserved;
  double value;
} GLTANG_FloatObject;

typedef struct GLTANG_ArrayStore {
  uint32_t kind;
  uint32_t reserved;
  uint64_t capacity;
  GLTANG_Value slots[];
} GLTANG_ArrayStore;

typedef struct GLTANG_ArrayObject {
  uint32_t kind;
  uint32_t reserved;
  uint64_t length;
  // A union so that the collector's barrier can be handed the address of a
  // plain `void *` slot without a pointer-type cast.
  union {
    void * raw;
    GLTANG_ArrayStore * typed;
  } store;
} GLTANG_ArrayObject;

typedef struct GLTANG_MapStore {
  uint32_t kind;
  uint32_t reserved;
  uint64_t capacity;     ///< Entries.
  uint64_t index_size;   ///< Index cells; a power of two.
  GLTANG_Value slots[];  ///< key, value for each entry; then the index.
} GLTANG_MapStore;

typedef struct GLTANG_MapObject {
  uint32_t kind;
  uint32_t reserved;
  uint64_t count;
  union {
    void * raw;
    GLTANG_MapStore * typed;
  } store;
} GLTANG_MapObject;

typedef struct GLTANG_ErrorObject {
  uint32_t kind;
  uint32_t error_kind;
  uint32_t function;
  uint32_t offset;
} GLTANG_ErrorObject;

static inline uint32_t gltang_object_kind(GLTANG_Value v) {
  return *(const uint32_t *)(const void *)(uintptr_t)v;
}
static inline void * gltang_object(GLTANG_Value v) {
  return (void *)(uintptr_t)v;
}
static inline GLTANG_Value gltang_value_of(const void * object) {
  return (GLTANG_Value)(uintptr_t)object;
}
static inline bool gltang_v_is_kind(GLTANG_Value v, GLTANG_ObjectKind kind) {
  return gltang_v_is_object(v) && gltang_object_kind(v) == (uint32_t)kind;
}
static inline uint32_t * gltang_map_index(const GLTANG_MapStore * store) {
  return (uint32_t *)(void *)((char *)store + sizeof(GLTANG_MapStore) + (size_t)store->capacity * 2u * sizeof(GLTANG_Value));
}

/** @brief The deepest a container nests for the recursive operations. */
#define GLTANG_MAX_VALUE_DEPTH 2048

// ---------------------------------------------------------------------------
// The execution
// ---------------------------------------------------------------------------


/** @brief Where the roots live in GLTANG_Execution::roots. */
#define GLTANG_ROOT_RESULT 0u
#define GLTANG_ROOT_OOM 1u
#define GLTANG_ROOT_FIXED 2u

struct GLTANG_Execution {
  GRCORE_Context * context;
  GRHEAP_Heap * heap;
  const GRCORE_Allocator * allocator;
  GRCORE_EngineId engine;
  GLTANG_Program * program;
  GLTANG_ExecutionState state;
  bool destroyed;
  bool unwinding;               ///< A runtime poll ordered the run to stop.

  // Roots, reported to the collector as precise slots: the result, the
  // preallocated Out of memory error, the program-scope variables, and the
  // constants that have been made into heap values.
  GLTANG_Value * roots;
  size_t root_count;
  GLTANG_Value * globals;       ///< Into roots.
  GLTANG_Value * constants;     ///< Into roots.

  // Temporary roots for an operation that builds more than one object.
  GLTANG_Value * temps;
  size_t temp_count;
  size_t temp_capacity;

  // The output, as typed segments.
  char * output;
  size_t output_length;
  size_t output_capacity;
  GLTANG_OutputSegment * segments;
  size_t segment_count;
  size_t segment_capacity;

  GLTANG_Resolver resolver;
  void * resolver_user;

  // Where the instruction being executed is, for errors and for natives.
  uint32_t current_function;
  uint32_t current_offset;
  uint64_t pending_fuel;        ///< Charged to the context at the next poll.
  uint64_t frames_unwound;
};

/** @brief The cost of each opcode; see bytecode.c. */
extern const uint32_t gltang_opcode_cost_table[GLTANG_OP_COUNT];

/** @brief The frame's header words. */
#define GLTANG_F_FUNCTION 0u
#define GLTANG_F_PC 1u
#define GLTANG_F_SP 2u
#define GLTANG_F_FLAGS 3u

/** @brief The engine's key: the execution is the context's keyed state. */
extern const GRCORE_Key gltang_execution_key;
/** @brief The engine's descriptor. */
extern const GRCORE_EngineDescriptor gltang_engine_descriptor;

// ---------------------------------------------------------------------------
// Status of an allocation, and the natives' poll
// ---------------------------------------------------------------------------

typedef enum {
  GLTANG_ST_OK = 0,
  GLTANG_ST_OOM,     ///< An allocation failed: make an Out of memory value.
  GLTANG_ST_UNWIND   ///< A poll ordered an unwind: return GLTANG_V_UNWIND.
} GLTANG_Status;

/** @brief Charges the pending fuel to the context. */
void gltang_vm_flush_fuel(GLTANG_Execution * exec);

/**
 * @brief The runtime poll for a native (AD-21): flushes fuel, charges `work`,
 *   polls allowing only continue and unwind.
 *
 * @return ::GLTANG_ST_OK, or ::GLTANG_ST_UNWIND (the execution is then
 *   marked as unwinding).
 */
GLTANG_Status gltang_vm_native_poll(GLTANG_Execution * exec, uint64_t work);

/** @brief Allocates a heap object of `bytes`; maps the failure to a status. */
GLTANG_Status gltang_vm_alloc(GLTANG_Execution * exec, const GRHEAP_Type * type, size_t bytes, void ** out);

/** @brief Keeps a value alive until the matching pop. */
bool gltang_vm_temp_push(GLTANG_Execution * exec, GLTANG_Value v);
void gltang_vm_temp_pop(GLTANG_Execution * exec);

// ---------------------------------------------------------------------------
// Values (value.c)
// ---------------------------------------------------------------------------

extern const GRHEAP_Type gltang_type_integer;
extern const GRHEAP_Type gltang_type_float;
extern const GRHEAP_Type gltang_type_string;
extern const GRHEAP_Type gltang_type_array;
extern const GRHEAP_Type gltang_type_array_store;
extern const GRHEAP_Type gltang_type_map;
extern const GRHEAP_Type gltang_type_map_store;
extern const GRHEAP_Type gltang_type_error;

GLTANG_ValueKind gltang_vm_kind(GLTANG_Value v);
bool gltang_vm_truthy(GLTANG_Value v);
bool gltang_vm_is_error(GLTANG_Value v);
bool gltang_vm_is_number(GLTANG_Value v);
bool gltang_vm_is_container(GLTANG_Value v);
int64_t gltang_vm_int(GLTANG_Value v);
double gltang_vm_float(GLTANG_Value v);
double gltang_vm_number(GLTANG_Value v);

/** @brief An integer value; boxes it if it does not fit inline. */
GLTANG_Value gltang_vm_make_int(GLTANG_Execution * exec, int64_t n);
GLTANG_Value gltang_vm_make_float(GLTANG_Execution * exec, double d);
/** @brief An error value with its origin. Never fails: falls back to the preallocated one. */
GLTANG_Value gltang_vm_make_error(GLTANG_Execution * exec, GLTANG_ErrorKind kind);
GLTANG_ErrorKind gltang_vm_error_kind(GLTANG_Value v);
/** @brief The value an allocation status stands for. */
GLTANG_Value gltang_vm_failure_value(GLTANG_Execution * exec, GLTANG_Status st);

// ---------------------------------------------------------------------------
// Strings (string.c)
// ---------------------------------------------------------------------------

static inline GLTANG_StringBlock * gltang_vm_string(GLTANG_Value v) {
  return (GLTANG_StringBlock *)gltang_object(v);
}
GLTANG_Status gltang_vm_string_alloc(GLTANG_Execution * exec, size_t segments, size_t bytes, size_t graphemes, GLTANG_StringBlock ** out);
/** @brief A string from UTF-8 bytes, breaking graphemes. Null on invalid text. */
GLTANG_Value gltang_vm_string_from_utf8(GLTANG_Execution * exec, const char * bytes, size_t length, GLTANG_String_Type type);
/** @brief A string from bytes that are ASCII without a carriage return. */
GLTANG_Value gltang_vm_string_from_ascii(GLTANG_Execution * exec, const char * bytes, size_t length, GLTANG_String_Type type);
/** @brief A heap string copied from a program's constant block. */
GLTANG_Value gltang_vm_string_from_block(GLTANG_Execution * exec, const GLTANG_StringBlock * block, size_t size);
GLTANG_Value gltang_vm_string_concat(GLTANG_Execution * exec, GLTANG_Value a, GLTANG_Value b);
GLTANG_Value gltang_vm_string_retag(GLTANG_Execution * exec, GLTANG_Value s, GLTANG_String_Type type);
GLTANG_Value gltang_vm_string_render(GLTANG_Execution * exec, GLTANG_Value s);
GLTANG_Value gltang_vm_string_grapheme(GLTANG_Execution * exec, GLTANG_Value s, size_t index);
GLTANG_Value gltang_vm_string_slice(GLTANG_Execution * exec, GLTANG_Value s, int64_t start, int64_t count, int64_t step);
bool gltang_vm_string_equal(GLTANG_Value a, GLTANG_Value b);
int gltang_vm_string_compare(GLTANG_Value a, GLTANG_Value b);
uint64_t gltang_vm_string_hash(const char * bytes, size_t length);

/**
 * @brief Writes a flat string's segments, each encoded per its tag, into a
 *   growing buffer; shared by the host's render and the string render.
 *
 * @param out Receives a gcu_malloc'd buffer (NUL-terminated) and its length.
 * @return true, or false when memory ran out.
 */
bool gltang_vm_render_block(const GLTANG_StringBlock * s, char ** out, size_t * out_length);

/**
 * @brief Encodes output segments the same way, into a gcu_malloc'd buffer.
 *
 * @return true, or false when memory ran out.
 */
bool gltang_vm_render_segments(const char * bytes, size_t length, const GLTANG_OutputSegment * segments, size_t segment_count, char ** out, size_t * out_length);

// ---------------------------------------------------------------------------
// Containers (container.c)
// ---------------------------------------------------------------------------

static inline GLTANG_ArrayObject * gltang_vm_array(GLTANG_Value v) {
  return (GLTANG_ArrayObject *)gltang_object(v);
}
static inline GLTANG_MapObject * gltang_vm_map(GLTANG_Value v) {
  return (GLTANG_MapObject *)gltang_object(v);
}

/** @brief An empty array with room for `capacity`. Fails as a value. */
GLTANG_Value gltang_vm_array_new(GLTANG_Execution * exec, size_t capacity);
/** @brief Fills a new array from `count` values; allocates nothing. */
void gltang_vm_array_fill(GLTANG_Execution * exec, GLTANG_Value array, const GLTANG_Value * elements, size_t count);
/** @brief Appends within capacity; allocates nothing. */
void gltang_vm_array_push(GLTANG_Execution * exec, GLTANG_Value array, GLTANG_Value v);
/** @brief Grows an array to `length` filling with null; returns the array or a failure value. */
GLTANG_Value gltang_vm_array_grow(GLTANG_Execution * exec, GLTANG_Value array, size_t length);
/** @brief Stores into an existing slot. */
void gltang_vm_array_set(GLTANG_Execution * exec, GLTANG_Value array, size_t index, GLTANG_Value v);
/** @brief An empty map with room for `capacity` entries. Fails as a value. */
GLTANG_Value gltang_vm_map_new(GLTANG_Execution * exec, size_t capacity);
/** @brief Fills a new map from `count` key, value pairs; allocates nothing. */
void gltang_vm_map_fill(GLTANG_Execution * exec, GLTANG_Value map, const GLTANG_Value * pairs, size_t count);
/** @brief The value for a string key; `*found` says whether the key is there. */
GLTANG_Value gltang_vm_map_get(GLTANG_Value map, const char * key, size_t length, bool * found);
/** @brief Sets a key; returns the map, or a failure value. */
GLTANG_Value gltang_vm_map_set(GLTANG_Execution * exec, GLTANG_Value map, GLTANG_Value key, GLTANG_Value v);
GLTANG_Value gltang_vm_deep_copy(GLTANG_Execution * exec, GLTANG_Value v);
GLTANG_Value gltang_vm_equal(GLTANG_Execution * exec, GLTANG_Value a, GLTANG_Value b);

// ---------------------------------------------------------------------------
// Text: sinks, number formatting, printing (text.c)
// ---------------------------------------------------------------------------

typedef enum {
  GLTANG_SINK_OUTPUT,  ///< The execution's output.
  GLTANG_SINK_STRING,  ///< A string being built, with graphemes.
  GLTANG_SINK_BUFFER   ///< A fixed buffer, as snprintf fills one.
} GLTANG_SinkMode;

typedef struct GLTANG_Sink {
  GLTANG_SinkMode mode;
  GLTANG_Execution * exec;
  // STRING
  char * bytes;
  size_t length;
  size_t capacity;
  uint32_t * boundaries;
  size_t boundary_count;
  size_t boundary_capacity;
  uint64_t * segments;
  size_t segment_count;
  size_t segment_capacity;
  size_t pace_since;  ///< Bytes appended since the last runtime poll.
  size_t limit;       ///< Stop rendering containers past this many bytes; 0 for none.
  // BUFFER
  char * buffer;
  size_t buffer_size;
  size_t needed;
} GLTANG_Sink;

void gltang_sink_init(GLTANG_Sink * sink, GLTANG_SinkMode mode, GLTANG_Execution * exec);
void gltang_sink_init_buffer(GLTANG_Sink * sink, char * buffer, size_t size);
void gltang_sink_free(GLTANG_Sink * sink);
GLTANG_Status gltang_sink_text(GLTANG_Sink * sink, const char * text, size_t length, GLTANG_String_Type type);
GLTANG_Status gltang_sink_string(GLTANG_Sink * sink, GLTANG_Value s);
/** @brief Makes the built string a heap string. */
GLTANG_Value gltang_sink_finish(GLTANG_Sink * sink);

size_t gltang_vm_format_integer(int64_t n, char * buffer);
size_t gltang_vm_format_float(double d, char * buffer, size_t size);

typedef enum {
  GLTANG_RENDER_PRINT,    ///< What `print` appends: functions and ordinary errors append nothing.
  GLTANG_RENDER_ELEMENT   ///< What a container shows for an element, and `as string`.
} GLTANG_RenderMode;

/** @brief Renders a value. Returns the status; too deep nesting is OK with `*too_deep`. */
GLTANG_Status gltang_vm_render(GLTANG_Sink * sink, GLTANG_Value v, GLTANG_RenderMode mode, int depth, bool * too_deep);
GLTANG_Value gltang_vm_op_print(GLTANG_Execution * exec, GLTANG_Value v);
GLTANG_Value gltang_vm_print_constant(GLTANG_Execution * exec, const GLTANG_StringBlock * s);
/** @brief The text a value becomes in a `+` with a string; null/function are `Not supported`. */
GLTANG_Value gltang_vm_to_string(GLTANG_Execution * exec, GLTANG_Value v);

// ---------------------------------------------------------------------------
// Operations (ops.c)
// ---------------------------------------------------------------------------

GLTANG_Value gltang_vm_op_binary(GLTANG_Execution * exec, GLTANG_Opcode op, GLTANG_Value a, GLTANG_Value b);
GLTANG_Value gltang_vm_op_neg(GLTANG_Execution * exec, GLTANG_Value v);
GLTANG_Value gltang_vm_op_cast(GLTANG_Execution * exec, GLTANG_Value v, GLTANG_Cast_Type type);
GLTANG_Value gltang_vm_op_index(GLTANG_Execution * exec, GLTANG_Value container, GLTANG_Value index);
GLTANG_Value gltang_vm_op_attr(GLTANG_Execution * exec, GLTANG_Value container, GLTANG_Value name);
GLTANG_Value gltang_vm_op_slice(GLTANG_Execution * exec, GLTANG_Value container, unsigned flags, const GLTANG_Value * parts);
GLTANG_Value gltang_vm_op_set_index(GLTANG_Execution * exec, GLTANG_Value container, GLTANG_Value index, GLTANG_Value v, bool adopt);
GLTANG_Value gltang_vm_op_set_attr(GLTANG_Execution * exec, GLTANG_Value container, GLTANG_Value name, GLTANG_Value v, bool adopt);
GLTANG_Value gltang_vm_op_adopt(GLTANG_Execution * exec, GLTANG_Value v);

// ---------------------------------------------------------------------------
// The execution (execution.c) and the resolver (resolve.c)
// ---------------------------------------------------------------------------

/** @brief The value of a program constant, made into a heap value on first use. */
GLTANG_Value gltang_vm_constant(GLTANG_Execution * exec, uint32_t index);
/** @brief Where the current instruction is, for a native's poll. */
GRCORE_Location gltang_vm_location(const GLTANG_Execution * exec);
/** @brief The one place a `use` is resolved; the library registry is not here yet. */
GLTANG_Value gltang_vm_resolve(GLTANG_Execution * exec, const GLTANG_StringBlock * path);
/** @brief The interpreter. */
GRCORE_Step gltang_vm_run(GLTANG_Execution * exec, GRCORE_Context * context);

#endif /* GHOTI_IO_GLTANG_VM_VM_INTERNAL_H */
