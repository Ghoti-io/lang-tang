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
 * The baseline JIT, as the interpreter sees it (AD-9, AD-13, AD-22).
 *
 * Nothing here is public. `JIT=no` compiles none of `src/jit/`, and the
 * interpreter reaches this header only behind `GLTANG_WITH_JIT`, so the
 * interpreter-only engine is the old loop and nothing more. The inside of the
 * module (the hotness table, the cache, the bytecode-to-IR compiler and the
 * poll helper) is in jit_internal.h, which is the only header that includes
 * runtime-jit.
 */

#ifndef GHOTI_IO_GLTANG_JIT_JIT_H
#define GHOTI_IO_GLTANG_JIT_JIT_H

#include <ghoti.io/lang-tang/macros.h>

#include "../vm/vm_internal.h"

/** @brief How an attempt to enter compiled code ended. */
typedef enum GLTANG_JitExit {
  GLTANG_JIT_NOT_ENTERED, ///< Nothing happened: the interpreter runs the function.
  GLTANG_JIT_RETURNED,    ///< The function returned a value; its frame is still pushed.
  GLTANG_JIT_DEOPTED,     ///< Compiled code left at an operation; the guest frame holds pc, sp and the slots.
  GLTANG_JIT_PAUSED,      ///< A poll in compiled code paused the run; the guest frame is current.
  GLTANG_JIT_UNWOUND      ///< A poll in compiled code unwound the run; the guest frame is current.
} GLTANG_JitExit;

/**
 * @brief Makes the execution's JIT state and registers the tier-up handler.
 *
 * Called while the context is parked, at creation and again by the setter if
 * it failed there. On any failure nothing is left registered and
 * `jit_threshold` stays 0.
 */
void gltang_jit_attach(GLTANG_Execution * exec);

/** @brief Frees the state and the compiled code. Called from the execution's teardown. */
void gltang_jit_release(GLTANG_Execution * exec);

/**
 * @brief Counts a poll of a function; at the threshold, queues it and posts
 *   the tier-up request, so that the poll about to be made runs the handler.
 *
 * `at_entry` says the poll is the function's entry poll. The main program's
 * top level is entered once, so a crossing in one of its loops would compile
 * code nothing can ever enter (compiled code is entered at a function's start
 * and never in the middle of a run): only its entry poll counts, which is what a
 * threshold of 1 makes tier up. Allocates nothing from the GC heap. If its own
 * bookkeeping cannot be allocated it turns tier-up off for the execution.
 */
void gltang_jit_note_poll(GLTANG_Execution * exec, uint64_t fword, bool at_entry);

/**
 * @brief Enters compiled code for the function whose entry poll has just
 *   continued, if there is any.
 *
 * Records a JIT activation, calls the code, leaves the record, and turns the
 * exit into the guest frame the interpreter expects (see ::GLTANG_JitExit).
 *
 * @param out_value Receives the returned value for ::GLTANG_JIT_RETURNED.
 */
GLTANG_JitExit gltang_jit_enter(GLTANG_Execution * exec, GRCORE_Context * context, uint64_t fword, GLTANG_Value * out_value);

/**
 * @brief Hands back the native-depth units the open JIT records cost, for the
 *   duration of a native call (AD-21, AD-28).
 *
 * An interpreted run opens no record, so a nesting of natives costs it fewer
 * units of the native-depth budget than the same nesting under compiled code,
 * which has a JIT record per compiled run. A native called from compiled code
 * gives those units back before it opens its own record and takes them again when
 * it returns, so the budget refuses at the same nesting in both tiers.
 *
 * @return How many units were handed back; pass it to ::gltang_jit_return_depth.
 */
size_t gltang_jit_lend_depth(GLTANG_Execution * exec);

/** @brief Takes back the units ::gltang_jit_lend_depth handed back. */
void gltang_jit_return_depth(GLTANG_Execution * exec, size_t lent);

/** @brief The run has unwound: clears a pending tier-up request. */
void gltang_jit_unwound(GLTANG_Execution * exec);

#endif /* GHOTI_IO_GLTANG_JIT_JIT_H */
