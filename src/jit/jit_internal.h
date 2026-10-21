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
 * The inside of the baseline JIT: the per-execution state, the compiler's entry
 * point, the hooks and the helpers compiled code calls. The only header that
 * includes runtime-jit, so tools/check-edges.sh can allow that edge for
 * src/jit/ alone.
 *
 * The design (AD-28, spec-runtime-calls story 8):
 *
 * - A compiled function is *callable*: it has an internal entry, an entry slot
 *   per (program, function) that the context owns, and its guest frame is
 *   pushed by whoever calls it, the interpreter or another compiled function's
 *   `push` hook. Compiled code keeps its values in its own native frame, in
 *   stack-mapped slots, and writes the guest frame only when a chain is
 *   rebuilt (the `deopt` hook).
 * - A compiled call is a call through the callee's slot, guarded on the callee
 *   value. A callee that cannot be compiled, a refused push and a native stack
 *   that would run out are exits at the call, which the interpreter then makes.
 * - Pending fuel lives in the execution (`pending_fuel`): compiled code adds to
 *   it directly and flushes it before every poll, so a frame state is exactly
 *   the guest frame and an exit loses nothing.
 * - The interpreter never runs as a C callee of compiled code, and nothing
 *   unwinds natively through a compiled frame.
 */

#ifndef GHOTI_IO_GLTANG_JIT_JIT_INTERNAL_H
#define GHOTI_IO_GLTANG_JIT_JIT_INTERNAL_H

#include <ghoti.io/lang-tang/macros.h>

#include "jit.h"

#include <ghoti.io/runtime-core/a/activation.h>
#include <ghoti.io/runtime-core/a/code.h>
#include <ghoti.io/runtime-core/a/codemeta.h>
#include <ghoti.io/runtime-core/a/deopt.h>
#include <ghoti.io/runtime-jit/backend.h>
#include <ghoti.io/runtime-jit/builder.h>
#include <ghoti.io/runtime-jit/code.h>
#include <ghoti.io/runtime-jit/ir.h>
#include <ghoti.io/runtime-jit/natives.h>

/**
 * @brief The allocator of everything the JIT keeps for an execution: its records,
 *   the IR and the code payloads. The group's, not the context's: compiled code is
 *   the engine's and not the program's, so it is not charged to the guest's memory
 *   budget (and the group's allocator outlives the context, which code retired
 *   after the execution is gone needs).
 */
#define GLTANG_JIT_ALLOCATOR(exec) grcore_group_allocator(grcore_context_group((exec)->context))

/** @brief The most interpreter slots a compiled function's frame may have; a bigger one is not compiled. */
#define GLTANG_JIT_MAX_SLOTS 1024u
/** @brief Deoptimizations after which a function's code is discarded and never compiled again. */
#define GLTANG_JIT_DEOPT_LIMIT 8u
/** @brief The most arguments of a compiled call (runtime-jit's limit is 16, one of them the entry flag). */
#define GLTANG_JIT_MAX_CALL_ARGS 15u
/* runtime-jit's builder takes at most 16 call arguments (GRJIT_BUILDER_MAX_ARGS, not public): the parameters and the hidden flag must fit. */
_Static_assert(GLTANG_JIT_MAX_CALL_ARGS + 1u <= 16u, "a compiled call's arguments and the entry flag must fit runtime-jit's call limit");

/** @brief Where a function is in its life in one execution. */
typedef enum GLTANG_JitFnState {
  GLTANG_JITFN_COLD = 0, ///< Counting polls.
  GLTANG_JITFN_QUEUED,   ///< Over the threshold; the tier-up handler will compile it.
  GLTANG_JITFN_COMPILED, ///< `code` is valid, registered, and in the function's slot.
  GLTANG_JITFN_NEVER     ///< Failed, declined or discarded: never compiled again; the slot is refused.
} GLTANG_JitFnState;

/** @brief One function's feedback, code and entry slot, in one execution. */
typedef struct GLTANG_JitFn {
  uint32_t polls;
  uint8_t state;       ///< A ::GLTANG_JitFnState.
  uint8_t deopts;
  uint32_t max_converting;  ///< The raw floats at one site of the compiled code, while `state` is COMPILED; 0 otherwise.
  GRCORE_Code * code;  ///< The cache's reference; compiled code's payload is a ::GLTANG_JitCode.
  GRCORE_EntrySlot * slot;  ///< Created when the function is first named; kept for the execution. Its state mirrors `state`.
  uintptr_t start;     ///< The registered range's start while `code` is registered; 0 otherwise.
} GLTANG_JitFn;

/** @brief The functions of one program an execution has run. */
typedef struct GLTANG_JitProgram {
  GLTANG_JitFn * fns;  ///< One per function; allocated at the program's first counted poll.
  uint32_t count;
  bool slots_ready;    ///< Every function of the program has an entry slot (`gltang_jit_prepare_slots`): generated code reads them through the tables without a test.
} GLTANG_JitProgram;

/** @brief The payload of a ::GRCORE_Code. */
typedef struct GLTANG_JitCode {
  GRJIT_Code * code;
  const GRCORE_Allocator * allocator;
  uint32_t frame_slots;       ///< The function's guest frame, in slots.
  uint32_t local_count;
  uint32_t parameter_count;
  uint32_t code_count;        ///< Bytecode words of the function (the size of `call_sites`' index space).
  uint8_t * call_sites;       ///< One bit per bytecode index: a compiled call is emitted there. Owned.
  uint8_t * expected_sites;   ///< One bit per bytecode index: the unconditional exit there boxes a raw float for its consumer (designed; it does not count toward the discard limit). Owned.
  uint8_t * native_sites;     ///< One bit per bytecode index: a native call (a library call or a member load) is emitted there. Owned.
  uint32_t max_converting;    ///< The most converting (raw `F64`) locations at any one site: the cells a frame of this function adds to the reservation (AD-27).
} GLTANG_JitCode;

/** @brief Why the last call exit happened, written by the hook that refused and read by the deopt hook. */
typedef enum GLTANG_JitRefusal {
  GLTANG_REFUSAL_NONE = 0,
  GLTANG_REFUSAL_REMEMBERED,   ///< The callee cannot be compiled (the compile hook said so).
  GLTANG_REFUSAL_PUSH,         ///< The push was refused: depth, memory, an argument error.
  GLTANG_REFUSAL_CALLEE_GUARD, ///< The callee value was not the function the site names.
  GLTANG_REFUSAL_NATIVE_GUARD, ///< The callee value was not the library native the site names.
  GLTANG_REFUSAL_OPERAND_GUARD ///< An operand of a value operation compiled as a call (`+`, `INDEX`, `SLICE`, `ATTR`) is a host-defined value, whose type's operation (a host callback) no compiled helper runs yet.
} GLTANG_JitRefusal;

/**
 * The natives compiled code calls (runtime-jit's `CALL_NATIVE`), registered once
 * per execution in a table, in this order: the call of a library native with 0 to
 * 15 arguments (the callee is the first parameter), the load of a `use`d path,
 * the `.name` of a library, the two output operations, `PRINT` (the value) and
 * `PRINT_CONST` (the constant's string block), and the value operations of
 * spec-template-compiled story 3: a string `CONST` (the constant's index and the
 * site), `+` past the small-integer path (the two operands), `INDEX` (the
 * container and the index), `SLICE` (its flags, the container and up to three
 * parts) and the `ATTR` of a value that is not a library (the value and the
 * name's constant); and those of story 4: a `CAST` the inline path does not do (the
 * value and the cast type: to bool, to string, and to int of a value that is not a
 * small integer or a float inside the small-integer range), the allocation of an
 * `ARRAY` or a `MAP` literal (its count; its elements are stored afterwards by
 * `gltang_jit_array_fill` and `gltang_jit_map_fill`, which are no GC points) and the
 * copy of a container `ADOPT` stores (the value).
 */
enum {
  GLTANG_JIT_NATIVE_CALL0 = 0,   ///< ... up to GLTANG_JIT_NATIVE_CALL0 + 15.
  GLTANG_JIT_NATIVE_USE = 16,
  GLTANG_JIT_NATIVE_ATTR = 17,
  GLTANG_JIT_NATIVE_PRINT = 18,
  GLTANG_JIT_NATIVE_PRINT_CONST = 19,
  GLTANG_JIT_NATIVE_CONST = 20,
  GLTANG_JIT_NATIVE_ADD = 21,
  GLTANG_JIT_NATIVE_INDEX = 22,
  GLTANG_JIT_NATIVE_SLICE = 23,
  GLTANG_JIT_NATIVE_VALUE_ATTR = 24,
  GLTANG_JIT_NATIVE_CAST = 25,
  GLTANG_JIT_NATIVE_ARRAY = 26,
  GLTANG_JIT_NATIVE_MAP = 27,
  GLTANG_JIT_NATIVE_ADOPT = 28,
  GLTANG_JIT_NATIVE_COUNT = 29
};

/**
 * @brief The elements one `gltang_jit_array_fill` stores, and the pairs one
 *   `gltang_jit_map_fill` stores: what fits runtime-jit's limit for a helper call
 *   (`GRJIT_Limits::max_call_arguments`, 6: the register arguments of every backend's
 *   convention) beside the execution and the container (and, for an array, the count). A
 *   longer literal is filled by several calls, in order, so no literal is left an exit
 *   for its length.
 */
#define GLTANG_JIT_FILL_ELEMENTS 3u
#define GLTANG_JIT_FILL_PAIRS 2u
_Static_assert(3u + GLTANG_JIT_FILL_ELEMENTS <= 6u && 2u + 2u * GLTANG_JIT_FILL_PAIRS <= 6u, "a fill call's arguments must fit runtime-jit's default helper-call limit");
/**
 * @brief The most native stack the thunks, the shared wrapper and the host function's own
 *   frame are taken to use before they call anything that checks: 16 KiB, because what a
 *   thunk reaches includes an allocation, and so a collection, and a runtime poll with its
 *   handlers, none of which checks the budget. The call site is an exit before it unless
 *   that much lies above the limit (runtime-jit's `stack_bytes`, at most 64 KiB).
 */
#define GLTANG_JIT_NATIVE_STACK 16384u

/** @brief One extension of the reservation, and the call that made it. */
typedef struct GLTANG_JitExtension {
  uint64_t level;  ///< The compiled-call depth it belongs to: 0 for an entry, otherwise `chain` as the push left it.
  uint64_t fword;  ///< The function whose frame it is for (the pop hook retracts only its own callee's).
  size_t cells;
} GLTANG_JitExtension;

/** @brief The execution's JIT state. */
typedef struct GLTANG_Jit {
  GLTANG_JitProgram * programs;  ///< Indexed by the program's index in the execution.
  size_t program_capacity;
  uint64_t * queue;              ///< Function words waiting to be compiled.
  size_t queue_count;
  size_t queue_capacity;
  GRCORE_RequestKind kind;       ///< The tier-up request kind.
  GRCORE_Stack * stack;          ///< The guest stack of `stack_context`, found once by the hooks (a context's stack is made when the engine registers and never replaced): the push and pop of every compiled call would otherwise look it up through the context's keyed slots.
  void * stack_context;          ///< The context `stack` was found for.
  GRCORE_Port * port;            ///< Posts it; taken at the first post.
  GRJIT_NativeTable * natives;   ///< The thunks of the library calls and loads, made at the first compile (gltang_jit_natives); NULL before, or if it could not be made (then no site is compiled as one).
  bool natives_tried;            ///< Whether gltang_jit_natives has tried to make `natives` (it tries once).
  GRCORE_DeoptReservation * reservation;  ///< Made once, empty; extended by each entry and compiled call by the callee's `max_converting` (AD-27).
  GLTANG_Value * pool;           ///< Float boxes made ahead of time, for `convert` to hand out: precise roots of the execution. The first `pool_count` are boxes not yet handed out.
  size_t pool_count;
  size_t pool_capacity;
  GLTANG_JitExtension * ext;     ///< What the entries and pushes open now extended the reservation by, innermost last (only the nonzero ones).
  size_t ext_count;
  size_t ext_capacity;
  size_t ext_floor;              ///< The first extension of the current run's calls: those below belong to the entry and to outer runs.
  uint64_t last_fword;           ///< The function the last counted poll was in, and its record: a loop polls one function over and over.
  GLTANG_JitFn * last_fn;
  size_t records_open;           ///< JIT activation records open now: each costs one unit of native depth that an interpreted run does not.
  size_t depth_lent;             ///< Of those, the units a native call has handed back for as long as it runs (see gltang_jit_lend_depth).
  uint64_t chain;                ///< Compiled calls open now (the push hook counts, the pop hook and a deopt take away).
  uint8_t refusal;               ///< A ::GLTANG_JitRefusal the deopt hook consumes.
  uint64_t exit_fword;           ///< The function whose frame was innermost at the last exit, for the discard limit.
  bool exit_counted;             ///< Whether that exit is of a kind that counts toward its function's discard limit.
  bool exit_expected;            ///< Whether that exit is a designed one: a raw float boxed for its consumer. Neither it nor an exit that counts nowhere is `exit_counted`.
  bool exit_valid;               ///< `exit_fword` was set by the deopt hook of this entry.
  bool test_calls_off;           ///< Test seam: compile no call sites.
  bool test_natives_off;         ///< Test seam: compile no library call or member load (as a backend that refuses natives would).
  bool test_fail_rebuild;        ///< Test seam: the deopt hook's rebuild fails.
  uint64_t test_fail_extend_at;  ///< Test seam: the Nth nonzero extension of the reservation is refused (0: never).
  uint64_t test_fail_pool_at;    ///< Test seam: the top-up of the pool after the Nth nonzero extension is refused (0: never).
  bool floaty;                   ///< Some compiled function holds raw floats (never cleared): until then a push and a pop do no float bookkeeping at all.
  uint64_t test_fail_stack_push_at; ///< Test seam: the Nth push hook that has extended the reservation finds its stack push refused (0: never).
  uint64_t test_stack_pushes;
  uint64_t test_fail_slot_at;    ///< Test seam: the Nth entry slot made is refused (0: never): the compile that needed it fails, counted.
  uint64_t test_slots_made;
  uint64_t test_failed_capacity; ///< The reservation's capacity and the extensions open right after that refusal was undone.
  uint64_t test_failed_extensions;
  uint64_t test_failed_count;
  uint64_t test_stray_words;     ///< Words in a guest frame's value slots, after an injected rebuild failure, that look like objects and are not in the heap.
  uint64_t test_extends;         ///< The nonzero extensions asked for so far, for the two seams above.
  bool gc_at_push;               ///< Test seam, for GC torture: the push hook collects after the push, as the frame-push GC point of AD-28.
} GLTANG_Jit;

/**
 * @brief Compiles one function of the execution.
 *
 * @param out Receives the compiled code with one reference (the caller's), or
 *   NULL when the function is declined: nothing is wrong, it is just not worth
 *   compiling (its first operation after the entry poll leaves compiled code,
 *   or it is too large). Written only on success.
 * @return ::GLTANG_OK (including a decline), or the error that stopped the
 *   compile: ::GLTANG_ERR_OOM, ::GLTANG_ERR_LIMIT, ::GLTANG_ERR_INTERNAL.
 */
GLTANG_Result gltang_jit_build(GLTANG_Execution * exec, uint32_t program_index, uint32_t function_index, GRCORE_Code ** out);

/**
 * @brief The entry slot of a function, made on first use and set to the
 *   function's state (a function that can never be compiled has a refused slot).
 * @return The slot, or NULL if it could not be made.
 */
GRCORE_EntrySlot * gltang_jit_slot_for(GLTANG_Execution * exec, uint64_t fword);

/**
 * @brief Makes the entry slot of every function of a program, once.
 *
 * Compiled calls find their callee's slot through the execution's tables, as the
 * code is shareable and holds no execution's address, and the last load of the
 * lookup is not tested: a program's functions all have a slot before any of its
 * functions is built. A slot that cannot be made fails the build (the caller is not
 * compiled, and the interpreter runs it), so no compiled call site can find an
 * empty one.
 *
 * @return False if a slot could not be made, or the program is not loaded.
 */
bool gltang_jit_prepare_slots(GLTANG_Execution * exec, uint32_t program_index);

/**
 * @brief Compiles a function, registers its code and installs it in its slot.
 *   On a decline or a failure the function is never compiled and its slot is
 *   refused. Not a GC point.
 * @return True if the function now has code in its slot.
 */
bool gltang_jit_compile_function(GLTANG_Execution * exec, uint64_t fword);

/** @brief Marks a function never-compile and refuses its slot. */
void gltang_jit_mark_never(GLTANG_Execution * exec, GLTANG_Jit * jit, GLTANG_JitFn * f);

/** @brief The function's record, or NULL. */
GLTANG_JitFn * gltang_jit_find_fn(GLTANG_Execution * exec, GLTANG_Jit * jit, uint64_t fword, bool create);

/**
 * @brief Extends the reservation by `cells` and tops the pool up to the new
 *   capacity, recording the extension at `level`. A GC point (the pool's boxes
 *   are heap objects, charged to the guest's memory budget). The callers ask only for
 *   `cells` > 0 (the entry and the push test it first); zero cells would be recorded
 *   and retracted like any other extension.
 * @return False, with nothing changed, if the reservation or the pool could not
 *   grow (counted in the statistics); the caller refuses the entry or the call.
 */
bool gltang_jit_extend(GLTANG_Execution * exec, size_t cells, uint64_t level, uint64_t fword);
/** @brief Gives back the extensions above `count`, innermost first. Never allocates. */
void gltang_jit_retract_to(GLTANG_Execution * exec, size_t count);
/** @brief Gives back the innermost extension if it is this run's and was made at `level` for `fword`. */
void gltang_jit_retract_level(GLTANG_Execution * exec, uint64_t level, uint64_t fword);

/** @brief The context's guest stack: a context's is made when the engine registers and is never replaced, so the first to need it keeps it (for that context: one handed another looks again). */
static inline GRCORE_Stack * gltang_jit_stack_of(GLTANG_Jit * jit, void * context) {
  if (jit->stack_context != context) {
    jit->stack = grcore_context_stack(context);
    jit->stack_context = context;
  }
  return jit->stack;
}

// The hooks (hooks.c), one set for every build.
extern const GRJIT_CallHooks gltang_jit_hooks;

/** @brief Called by compiled code before a poll: charges what compiled code has added to the execution's pending fuel to the context. No GC point. */
void gltang_jit_flush(GLTANG_Execution * exec);
/** @brief Called by compiled code when the callee-value guard of a call site failed, just before its exit. No GC point. */
void gltang_jit_note_callee_guard(GLTANG_Execution * exec);
/** @brief The same for the guard of a library call. No GC point. */
void gltang_jit_note_native_guard(GLTANG_Execution * exec);
/** @brief The same for the guard of a value operation that its operand is not a host-defined value. No GC point. */
void gltang_jit_note_operand_guard(GLTANG_Execution * exec);
/** @brief Whether `a` or `b` is a host-defined value (1, the refusal noted as gltang_jit_note_operand_guard does) or neither (0): the test before `+`'s slow path. A leaf: no GC point. */
uint64_t gltang_jit_operands_custom(GLTANG_Execution * exec, uint64_t a, uint64_t b);
/**
 * @brief `CLEAR_RESULT` from compiled code: the interpreter's own
 *   `gltang_vm_set_result(exec, null, false)`. It neither allocates in the heap nor polls,
 *   so it is not a GC point: a lost result it replaces is entered in the error list (the
 *   execution's own memory) and marked logged in its error object's flags, a plain word.
 */
void gltang_jit_clear_result(GLTANG_Execution * exec);
/**
 * @brief `SET_RESULT` from compiled code: the interpreter's own
 *   `gltang_vm_set_result(exec, value, listed)`. An earlier result that was lost is
 *   entered in the error list as discarded, and a listed error result that is not yet
 *   logged is marked lost. Not a GC point, for the reason `CLEAR_RESULT` is not: it
 *   allocates only the error list's entries, through the execution's allocator, and
 *   writes a flag word of an error object; it never polls.
 */
void gltang_jit_set_result(GLTANG_Execution * exec, uint64_t value, uint64_t listed);
/**
 * @brief `DISCARD` from compiled code: an error value is entered in the error list as
 *   discarded, at the position `function`, `offset` (stored only for an error, as the
 *   interpreter's SYNC is). Not a GC point, as above.
 */
void gltang_jit_discard(GLTANG_Execution * exec, uint64_t value, uint64_t function, uint64_t offset);
/**
 * @brief The elements of an `ARRAY` literal from compiled code, `count` of them (at most
 *   ::GLTANG_JIT_FILL_ELEMENTS; the rest are zero and not read), appended to the array the
 *   native `ARRAY` made: the interpreter's own `gltang_vm_array_fill`. Not a GC point: the
 *   array was made with room for every element, so a store neither allocates nor polls (it
 *   goes through the barrier). A value that is not an array (the failure the allocation
 *   made, which is then the literal's value) is left as it is, as the interpreter leaves it.
 */
void gltang_jit_array_fill(GLTANG_Execution * exec, uint64_t array, uint64_t count, uint64_t e0, uint64_t e1, uint64_t e2);
/**
 * @brief The same for a `MAP` literal: one or two key and value pairs (a second key of 0,
 *   which no string is, means one), through the interpreter's `gltang_vm_map_fill`, so a
 *   key that comes again replaces the value (the last wins), across calls too. Not a GC point.
 * @return The literal's value: the map, or the out-of-memory root where a key is that error (a
 *   constant the memory budget refused), or whatever non-map value it was handed (an earlier
 *   failure), which compiled code then takes as the container.
 */
uint64_t gltang_jit_map_fill(GLTANG_Execution * exec, uint64_t map, uint64_t k0, uint64_t v0, uint64_t k1, uint64_t v1);
/**
 * @brief The fast path of a compiled `PRINT_CONST` of the program's text `block`
 *   (spec-template-compiled story 4d): gltang_vm_print_constant_fast, and then the operation's
 *   fuel and counters. Not a GC point: nothing on it allocates or polls.
 * @return 1 if the text was printed; 0 if nothing was done, and compiled code then makes the
 *   native-table call of `PRINT_CONST`.
 */
uint64_t gltang_jit_print_const_fast(GLTANG_Execution * exec, uint64_t block);
/** @brief The same for a compiled `PRINT` of `value` (gltang_vm_print_string_fast): 1 if printed, whose value is then null. */
uint64_t gltang_jit_print_fast(GLTANG_Execution * exec, uint64_t value);
/**
 * @brief The fast path of a compiled `ATTR` by the constant `constant` (its program block `block`,
 *   whose bytes hash to `hash`) of a value not known to be a library: gltang_vm_map_attr_fast,
 *   then the operation's fuel and counters. Not a GC point.
 * @return The attribute, or GLTANG_V_UNWIND if nothing was done (compiled code then makes the
 *   native-table call of `ATTR`).
 */
uint64_t gltang_jit_map_attr_fast(GLTANG_Execution * exec, uint64_t container, uint64_t constant, uint64_t block, uint64_t hash);
/** @brief Whether a callee value is a library native of `member` (and not an engine builtin). A leaf: no GC point. */
uint64_t gltang_jit_callee_is_native(uint64_t callee, uint64_t member);

/**
 * @brief Makes the table of the natives compiled code calls (natives.c).
 * @return ::GRJIT_OK or the table's error; on an error nothing is left.
 */
GRJIT_Result gltang_jit_natives_create(GLTANG_Execution * exec, GRJIT_NativeTable ** out);
/**
 * @brief The execution's table of natives, made by the first call (the first compile), not when
 *   the JIT is attached: an execution that compiles nothing (most of a short script's, or of a
 *   template run once by its host) does not pay for thirty-odd registrations. NULL if it could not
 *   be made; it is tried once.
 */
GRJIT_NativeTable * gltang_jit_natives(GLTANG_Execution * exec);
/**
 * @brief The poll slow path of compiled code, the one GC point of a function
 *   that makes no call. Returns 0 to continue, 1 after a pause verdict, 2 after an
 *   unwind verdict.
 *
 * It does not touch the guest frame: the polling frame is paired with its guest
 * frame, the collector updates the compiled frame through its stack map and does
 * not scan the guest one, so a write-back of the guest copy would put stale
 * references over updated ones.
 */
uint32_t gltang_jit_poll(void * context, uint64_t function, uint64_t offset);

#endif /* GHOTI_IO_GLTANG_JIT_JIT_INTERNAL_H */
