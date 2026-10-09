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

/** @brief The most interpreter slots a compiled function's frame may have; a bigger one is not compiled. */
#define GLTANG_JIT_MAX_SLOTS 1024u
/** @brief Deoptimizations after which a function's code is discarded and never compiled again. */
#define GLTANG_JIT_DEOPT_LIMIT 8u
/** @brief The most arguments of a compiled call (runtime-jit's limit is 16, one of them the entry flag). */
#define GLTANG_JIT_MAX_CALL_ARGS 15u

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
  GRCORE_Code * code;  ///< The cache's reference; compiled code's payload is a ::GLTANG_JitCode.
  GRCORE_EntrySlot * slot;  ///< Created when the function is first named; kept for the execution. Its state mirrors `state`.
  uintptr_t start;     ///< The registered range's start while `code` is registered; 0 otherwise.
} GLTANG_JitFn;

/** @brief The functions of one program an execution has run. */
typedef struct GLTANG_JitProgram {
  GLTANG_JitFn * fns;  ///< One per function; allocated at the program's first counted poll.
  uint32_t count;
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
} GLTANG_JitCode;

/** @brief Why the last call exit happened, written by the hook that refused and read by the deopt hook. */
typedef enum GLTANG_JitRefusal {
  GLTANG_REFUSAL_NONE = 0,
  GLTANG_REFUSAL_REMEMBERED,   ///< The callee cannot be compiled (the compile hook said so).
  GLTANG_REFUSAL_PUSH,         ///< The push was refused: depth, memory, an argument error.
  GLTANG_REFUSAL_CALLEE_GUARD  ///< The callee value was not the function the site names.
} GLTANG_JitRefusal;

/** @brief The execution's JIT state. */
typedef struct GLTANG_Jit {
  GLTANG_JitProgram * programs;  ///< Indexed by the program's index in the execution.
  size_t program_capacity;
  uint64_t * queue;              ///< Function words waiting to be compiled.
  size_t queue_count;
  size_t queue_capacity;
  GRCORE_RequestKind kind;       ///< The tier-up request kind.
  GRCORE_Port * port;            ///< Posts it; taken at the first post.
  GRCORE_DeoptReservation * reservation;  ///< Made once; extended by each compiled call (by zero here: no converting location).
  uint64_t last_fword;           ///< The function the last counted poll was in, and its record: a loop polls one function over and over.
  GLTANG_JitFn * last_fn;
  uint64_t chain;                ///< Compiled calls open now (the push hook counts, the pop hook and a deopt take away).
  uint8_t refusal;               ///< A ::GLTANG_JitRefusal the deopt hook consumes.
  uint64_t exit_fword;           ///< The function whose frame was innermost at the last exit, for the discard limit.
  bool exit_counted;             ///< Whether that exit counts toward its discard limit.
  bool exit_valid;               ///< `exit_fword` was set by the deopt hook of this entry.
  bool test_calls_off;           ///< Test seam: compile no call sites.
  bool test_fail_rebuild;        ///< Test seam: the deopt hook's rebuild fails.
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

// The hooks (hooks.c), one set for every build.
extern const GRJIT_CallHooks gltang_jit_hooks;

/** @brief Called by compiled code before a poll: charges what compiled code has added to the execution's pending fuel to the context. No GC point. */
void gltang_jit_flush(GLTANG_Execution * exec);
/** @brief Called by compiled code when the callee-value guard of a call site failed, just before its exit. No GC point. */
void gltang_jit_note_callee_guard(GLTANG_Execution * exec);
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
