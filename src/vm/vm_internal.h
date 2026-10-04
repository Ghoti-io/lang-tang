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
#include <ghoti.io/cutil/random.h>
#include <ghoti.io/lang-tang/execution.h>
#include <ghoti.io/lang-tang/library.h>
#include <ghoti.io/lang-tang/seeds.h>
#include <ghoti.io/lang-tang/value.h>
#include <ghoti.io/runtime-core/a/budget_scope.h>
#include <ghoti.io/runtime-core/a/engine.h>
#include <ghoti.io/runtime-core/a/frame.h>
#include <ghoti.io/runtime-core/a/stack.h>
#include <ghoti.io/runtime-core/a/unwind.h>
#include <ghoti.io/runtime-core/b/budget.h>
#include <ghoti.io/runtime-core/b/context.h>
#include <ghoti.io/runtime-core/b/poll.h>
#include <ghoti.io/runtime-core/b/request.h>
#include <ghoti.io/runtime-core/b/roots.h>
#include <ghoti.io/runtime-core/b/run.h>
#include <ghoti.io/runtime-core/b/snapshot.h>
#include <ghoti.io/runtime-heap/heap.h>
#include <ghoti.io/runtime-heap/image.h>
#include <ghoti.io/runtime-heap/roots.h>
#include <ghoti.io/runtime-heap/store.h>
#include <ghoti.io/runtime-heap/type.h>
#include "../compile/program_internal.h"
#include "../library/library_internal.h"
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
  GLTANG_OBJ_ERROR,         ///< An error value.
  GLTANG_OBJ_LIBRARY,       ///< A library: a pointer to a sealed GLTANG_Library.
  GLTANG_OBJ_NATIVE,        ///< A native function, perhaps bound to a value.
  GLTANG_OBJ_TEMPLATE,      ///< A template: a pointer to its library member.
  GLTANG_OBJ_RNG            ///< A random number generator, its state inline.
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

/** @brief Set in GLTANG_ErrorObject::flags once the error is in the error list. */
#define GLTANG_ERROR_FLAG_LOGGED 1u

typedef struct GLTANG_ErrorObject {
  uint32_t kind;
  uint32_t error_kind;
  uint32_t function;
  uint32_t offset;
  uint32_t program;  ///< Index into the execution's programs: where `function` is.
  uint32_t flags;
} GLTANG_ErrorObject;

/** @brief A library as a value. The library is kept alive by the execution. */
typedef struct GLTANG_LibraryObject {
  uint32_t kind;
  uint32_t reserved;
  const GLTANG_Library * library;
} GLTANG_LibraryObject;

/**
 * @brief A native function as a value: a host function (`member`), or one of the
 *   engine's own (`builtin`), possibly bound to a value (a generator's method).
 */
typedef struct GLTANG_NativeObject {
  uint32_t kind;
  uint32_t builtin;                     ///< GLTANG_BuiltinId, or 0 for a host function.
  const GLTANG_LibraryMember * member;  ///< The host function's member.
  GLTANG_Value bound;                   ///< Traced; the receiver of a method.
} GLTANG_NativeObject;

/** @brief A template as a value. */
typedef struct GLTANG_TemplateObject {
  uint32_t kind;
  uint32_t reserved;
  const GLTANG_LibraryMember * member;
} GLTANG_TemplateObject;

/** @brief A generator: std::mt19937_64's state, inline. */
typedef struct GLTANG_RngObject {
  uint32_t kind;
  uint32_t is_global;
  uint64_t seed;
  GCU_Random_MT64_State state;
} GLTANG_RngObject;

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

/** @brief What a host function is given; see library.h. */
struct GLTANG_NativeCall {
  GLTANG_Execution * exec;
  const GLTANG_Value * args;    ///< Into the operand stack; valid for the call (a host function cannot reach a GC point).
  size_t argc;
  enum {
    GLTANG_CALL_NONE = 0, GLTANG_CALL_NULL, GLTANG_CALL_BOOL, GLTANG_CALL_INTEGER,
    GLTANG_CALL_FLOAT, GLTANG_CALL_STRING, GLTANG_CALL_ERROR
  } set;
  bool boolean;
  int64_t integer;
  double number;
  char * text;                  ///< STRING: a copy, cutil's allocator.
  size_t length;
  GLTANG_String_Type encoding;
  GLTANG_ErrorKind error;
  bool failed_to_copy;
};

/** @brief The deepest a container nests for the recursive operations. */
#define GLTANG_MAX_VALUE_DEPTH 2048

// ---------------------------------------------------------------------------
// The execution
// ---------------------------------------------------------------------------


/** @brief Where the roots live in GLTANG_Execution::roots. */
#define GLTANG_ROOT_OOM 0u
#define GLTANG_ROOT_RANDOM 1u
#define GLTANG_ROOT_FIXED 2u

/** @brief An entry of the error list, and the chain of template calls above it. */
typedef struct GLTANG_ErrorRecord {
  GLTANG_ErrorEntry entry;
  GLTANG_ErrorLink * chain;     ///< Owned; entry.chain_count links, outermost first.
  char * text;                  ///< Owned, or NULL: the strings of a record a snapshot restored (the entry and the chain point into it).
} GLTANG_ErrorRecord;

/** @brief An output: bytes, and the typed segments that cover them. */
typedef struct GLTANG_OutBuf {
  char * bytes;
  size_t length;
  size_t capacity;
  GLTANG_OutputSegment * segments;
  size_t segment_count;
  size_t segment_capacity;
  size_t committed;             ///< `length` as of the last print that finished: a cut lands between prints.
} GLTANG_OutBuf;

/** @brief One program an execution has run frames of: the main one, or a template's. */
typedef struct GLTANG_ProgramEntry {
  GLTANG_Program * program;     ///< Retained.
  GLTANG_Value * constants;     ///< The program's constants made into heap values; roots.
} GLTANG_ProgramEntry;

/**
 * The engine's natives, from natives.def: who is polling the runtime. The id
 * is the name the native gate reads in the sources (see natives.def), and a
 * name that is not in the list does not compile. Each poll is counted under
 * its id, which is how the gate knows which native a poll was made for
 * (gltang_execution_native_polls).
 */
typedef enum GLTANG_NativeId {
#define GLTANG_NATIVE(id, unbounded, text) GLTANG_NATIVE_##id,
#include "natives.def"
#undef GLTANG_NATIVE
  GLTANG_NATIVE_COUNT
} GLTANG_NativeId;

/**
 * @brief One running program: the main one, or a template call.
 *
 * Everything per call that is not on the guest stack lives here, in the
 * execution, so a paused context migrates with it (AD-20) and the collector
 * finds the values in it (a root source).
 */
typedef struct GLTANG_Activation {
  struct GLTANG_Activation * parent;
  size_t depth;                 ///< 0 for the main program; a frame's flags word holds its activation's.
  uint32_t program_index;       ///< Into GLTANG_Execution::programs.
  GLTANG_Value result;          ///< The value of the last top-level statement; a root.
  bool result_lost;             ///< `result` is an error no variable holds: listed if replaced.
  GLTANG_Value * globals;       ///< The program-scope variables; roots.
  bool owns_globals;
  GLTANG_OutBuf out;
  uint32_t call_function;       ///< Where in the parent's program this call was made.
  uint32_t call_offset;
  const char * name;            ///< The template's name.
  uint64_t scope_id;            ///< The call's budget scope; 0 for the main program.
  GLTANG_ScopePolicy policy;
  size_t temp_base;             ///< GLTANG_Execution::temp_count when the call began.
} GLTANG_Activation;

struct GLTANG_Execution {
  GRCORE_Context * context;
  GRHEAP_Heap * heap;
  const GRCORE_Allocator * allocator;
  GRCORE_EngineId engine;
  GLTANG_Program * program;     ///< The main program.
  GLTANG_ExecutionState state;
  bool destroyed;
  bool unwinding;               ///< A runtime poll ordered the run to stop.
  bool in_host;                 ///< Inside a native function or a factory.

  // Roots, reported to the collector as precise slots: the Out of memory
  // error, the execution's random generator, the main program's variables,
  // and the constants that have been made into heap values.
  GLTANG_Value * roots;
  size_t root_count;

  // The programs frames belong to, and the running programs.
  GLTANG_ProgramEntry * programs;
  size_t program_count;
  size_t program_capacity;
  GLTANG_Activation main_act;
  GLTANG_Activation * act;      ///< The innermost running program.
  GLTANG_Value * globals;       ///< act's.
  GLTANG_Value * constants;     ///< act's program's.
  GLTANG_OutBuf * out;          ///< act's.

  // Temporary roots for an operation that builds more than one object.
  GLTANG_Value * temps;
  size_t temp_count;
  size_t temp_capacity;

  // The error list (CAP-1), and the switches that shape it.
  GLTANG_ErrorRecord * errors;
  size_t error_count;
  size_t error_capacity;
  size_t error_limit;           ///< The most entries kept.
  uint64_t errors_dropped;
  bool log_all_errors;
  bool halt_on_error;
  bool statement_polls;         ///< `LINE` polls like `POLL` (the host asked for it; default off).
  bool halted;                  ///< The first error has ended the run.
  bool halt_registered;
  GRCORE_RequestKind halt_kind;
  GRCORE_Port * port;

  // What the host attached.
  GLTANG_Library * libraries;
  GLTANG_SeedSequence * seeds;
  const char * name;            ///< The main template's name, for the error list; NULL for the default.
  char * name_storage;          ///< Owns `name`.

  // Where the instruction being executed is, for errors and for natives.
  uint32_t current_function;
  uint32_t current_offset;
  uint64_t pending_fuel;        ///< Charged to the context at the next poll.
  uint64_t frames_unwound;
  uint64_t native_polls[GLTANG_NATIVE_COUNT]; ///< Polls made for each native.

#ifdef GLTANG_WITH_JIT
  // The baseline JIT (src/jit/, story 15). Per-execution, so feedback lives
  // with the context (AD-22). `jit_threshold` is 0 when tier-up is off, which
  // is also what a failed attach leaves: the JIT is an optimisation, never a
  // reason for an execution not to exist.
  struct GLTANG_Jit * jit;
  uint32_t jit_threshold;
  GLTANG_JitStats jit_stats;
#endif
};

/**
 * @brief The function word of a frame header and of a poll identity: the
 *   function index in the low 32 bits and the program's index (into
 *   GLTANG_Execution::programs; 0 is the main program) in the high 32.
 */
#define GLTANG_FN_WORD(program, function) (((uint64_t)(program) << 32) | (uint64_t)(uint32_t)(function))
#define GLTANG_FN_PROGRAM(word) ((uint32_t)((word) >> 32))
#define GLTANG_FN_INDEX(word) ((uint32_t)(word))

/** @brief The program at an index, or NULL. */
static inline const GLTANG_Program * gltang_exec_program(const GLTANG_Execution * exec, uint32_t index) {
  return index < exec->program_count ? exec->programs[index].program : NULL;
}

/** @brief The program the innermost activation is running. */
static inline const GLTANG_Program * gltang_exec_current_program(const GLTANG_Execution * exec) {
  return exec->programs[exec->act->program_index].program;
}

/** @brief The cost of each opcode; see bytecode.c. */
extern const uint32_t gltang_opcode_cost_table[GLTANG_OP_COUNT];

/** @brief The frame's header words. */
#define GLTANG_F_FUNCTION 0u
#define GLTANG_F_PC 1u
#define GLTANG_F_SP 2u
#define GLTANG_F_FLAGS 3u

// Snapshots (snapshot.c): the per-object hooks of the three host-pointer types,
// and the execution key's hooks.
GRHEAP_Result gltang_vm_library_snapshot(GRCORE_Context * context, void * payload, GRHEAP_ImageWriter * writer);
GRHEAP_Result gltang_vm_library_restore(GRCORE_Context * context, void * payload, GRHEAP_ImageReader * reader, void * user);
GRHEAP_Result gltang_vm_native_snapshot(GRCORE_Context * context, void * payload, GRHEAP_ImageWriter * writer);
GRHEAP_Result gltang_vm_native_restore(GRCORE_Context * context, void * payload, GRHEAP_ImageReader * reader, void * user);
GRHEAP_Result gltang_vm_template_snapshot(GRCORE_Context * context, void * payload, GRHEAP_ImageWriter * writer);
GRHEAP_Result gltang_vm_template_restore(GRCORE_Context * context, void * payload, GRHEAP_ImageReader * reader, void * user);
GRCORE_Result gltang_vm_exec_snapshot(GRCORE_Context * context, void * value, GRCORE_SnapshotWriter * writer);
GRCORE_Result gltang_vm_exec_restore(GRCORE_Context * context, void * value, GRCORE_SnapshotReader * reader, void * env, GRCORE_RestoreMode mode);
GRCORE_Result gltang_vm_exec_settle(GRCORE_Context * context, void * value, void * env, GRCORE_SettleMode mode);

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

/** @brief The execution registered in a context, or NULL. */
GLTANG_Execution * gltang_vm_execution_of(const GRCORE_Context * context);

/**
 * @brief The runtime poll for a native (AD-21): flushes fuel, charges `work`,
 *   polls allowing only continue and unwind.
 *
 * @return ::GLTANG_ST_OK, or ::GLTANG_ST_UNWIND (the execution is then
 *   marked as unwinding).
 */
GLTANG_Status gltang_vm_native_poll(GLTANG_Execution * exec, uint64_t work);

/** Polls the runtime for `native`, charging `work` fuel (at least the poll). */
GLTANG_Status gltang_vm_native_poll_as(GLTANG_Execution * exec, GLTANG_NativeId native, uint64_t work);

/** A poll that names its native. */
#define GLTANG_NATIVE_POLL(exec, native, work) gltang_vm_native_poll_as((exec), GLTANG_NATIVE_##native, (work))

/** A pacer's initialiser, naming the native it paces. */
#define GLTANG_PACER(exec, native) {(exec), 0, GLTANG_NATIVE_##native}

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
extern const GRHEAP_Type gltang_type_library;
extern const GRHEAP_Type gltang_type_native;
extern const GRHEAP_Type gltang_type_template;
extern const GRHEAP_Type gltang_type_rng;

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
/** @brief The attribute rule on a name given as bytes. */
GLTANG_Value gltang_vm_op_attr_named(GLTANG_Execution * exec, GLTANG_Value container, const char * name, size_t length);
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
// ---------------------------------------------------------------------------
// Libraries, native functions, generators (libvalue.c)
// ---------------------------------------------------------------------------

/** @brief Resolves the dotted path of a `use`: the execution's libraries, the program's, the built-ins. */
GLTANG_Value gltang_vm_resolve(GLTANG_Execution * exec, const GLTANG_StringBlock * path);
/** @brief The value of a library member (a factory is called here). */
GLTANG_Value gltang_vm_member_value(GLTANG_Execution * exec, const GLTANG_LibraryMember * member);
/** @brief `library.name`: the member, or `Not implemented`. */
GLTANG_Value gltang_vm_library_attr(GLTANG_Execution * exec, GLTANG_Value library, const char * name, size_t length);
/** @brief `rng.name`: `next_int`, `next_float`, `next_bool` draw; `set_seed` is a method. */
GLTANG_Value gltang_vm_rng_attr(GLTANG_Execution * exec, GLTANG_Value rng, const char * name, size_t length);
/** @brief Calls a native function value (a host's, or one of the engine's). */
GLTANG_Value gltang_vm_call_native(GLTANG_Execution * exec, GLTANG_Value callee, size_t argc, const GLTANG_Value * args);
/** @brief The member of a template value. */
static inline const GLTANG_LibraryMember * gltang_vm_template_member(GLTANG_Value v) {
  return ((const GLTANG_TemplateObject *)gltang_object(v))->member;
}

// ---------------------------------------------------------------------------
// The error list (errorlist.c)
// ---------------------------------------------------------------------------

/** @brief The default cap on the error list. */
#define GLTANG_ERROR_LIMIT_DEFAULT 1024u

/** @brief The name of the main program for the error list. */
const char * gltang_vm_main_name(const GLTANG_Execution * exec);
/** @brief The name of the template an activation runs. */
const char * gltang_vm_activation_name(const GLTANG_Execution * exec, const GLTANG_Activation * act);
/**
 * @brief Enters a swallowed error value in the list, at most once per value.
 *
 * `source` is the activation the error belongs to: its template, and the chain
 * of calls above it. Nothing happens for a value that is not an error, a
 * marker, or one already entered. Never fails: an entry that cannot be made is
 * counted as dropped.
 */
void gltang_vm_error_swallowed(GLTANG_Execution * exec, GLTANG_ErrorHow how, GLTANG_Value error, const GLTANG_Activation * source);
/** @brief Enters the stop of a template call by its budget scope. */
void gltang_vm_error_scope_limit(GLTANG_Execution * exec, const GLTANG_Activation * callee);
/**
 * @brief Called by every error creation: logs it when the host asked, and
 *   halts the run when the host asked for that.
 *
 * @return ::GLTANG_V_UNWIND when the run is to end, else `error`.
 */
GLTANG_Value gltang_vm_error_created(GLTANG_Execution * exec, GLTANG_Value error);
/** @brief Frees the list. */
void gltang_vm_errors_free(GLTANG_Execution * exec);
/** @brief The error value a scope-limited template call stands for: logged already, never halts. */
GLTANG_Value gltang_vm_make_limit_error(GLTANG_Execution * exec);
/** @brief Replaces an activation's result; a lost error it held is entered first. */
void gltang_vm_set_result(GLTANG_Execution * exec, GLTANG_Value v, bool listed);

// ---------------------------------------------------------------------------
// Template calls (template.c)
// ---------------------------------------------------------------------------

/** @brief The index of a program in the execution's table; adds it (retained) on first use. False when memory runs out. */
bool gltang_vm_program_index(GLTANG_Execution * exec, GLTANG_Program * program, uint32_t * out_index);
/**
 * @brief Makes the record of a template call: fresh variables, an empty output.
 *
 * Not yet the innermost activation: ::gltang_vm_activation_enter does that once
 * the callee's frame is pushed. NULL when memory runs out.
 */
GLTANG_Activation * gltang_vm_activation_new(GLTANG_Execution * exec, uint32_t program_index, const char * name);
/** @brief Makes `act` the innermost activation, a child of the current one. */
void gltang_vm_activation_enter(GLTANG_Execution * exec, GLTANG_Activation * act, uint32_t call_function, uint32_t call_offset);
/** @brief Makes the parent of `act` (the innermost) the innermost again; `act` is not freed. */
void gltang_vm_activation_leave(GLTANG_Execution * exec, GLTANG_Activation * act);
/** @brief Frees a record that has left (or never entered). */
void gltang_vm_activation_free(GLTANG_Execution * exec, GLTANG_Activation * act);
/**
 * @brief An output's segments, up to `length` bytes, as a string with every
 *   segment's encoding tag kept.
 */
GLTANG_Value gltang_vm_output_string(GLTANG_Execution * exec, const GLTANG_OutBuf * out, size_t length);

/** @brief Makes `act` the innermost activation: points the execution's variables, constants and output at it. */
void gltang_vm_set_activation(GLTANG_Execution * exec, GLTANG_Activation * act);
/** @brief Frees an output's buffers. */
void gltang_vm_outbuf_free(GLTANG_Execution * exec, GLTANG_OutBuf * out);
/** @brief The interpreter. */
GRCORE_Step gltang_vm_run(GLTANG_Execution * exec, GRCORE_Context * context);

#endif /* GHOTI_IO_GLTANG_VM_VM_INTERNAL_H */
