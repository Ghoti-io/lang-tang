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
 * point and the helpers compiled code calls. The only header that includes
 * runtime-jit, so tools/check-edges.sh can allow that edge for src/jit/ alone.
 *
 * The one design that everything here follows (story 15): compiled code runs in
 * a guest frame the interpreter has already pushed, and updates it only at a
 * poll's slow path and when it leaves. Between those the guest frame is stale,
 * which is harmless because compiled code reaches no GC point in between: it
 * calls no allocating, polling or guest-calling helper, and a `CALL` is a
 * deoptimization exit. So no native frame is ever scanned by the collector, no
 * raw reference is held across a GC point (AD-17), and a paused context holds
 * only interpreter frames (AD-8).
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

/** @brief The most interpreter slots (plus the fuel slot) a compiled function may have; a bigger one is not compiled. */
#define GLTANG_JIT_MAX_SLOTS 1024u
/** @brief Deoptimizations after which a function's code is discarded and never compiled again. */
#define GLTANG_JIT_DEOPT_LIMIT 8u

/** @brief Where a function is in its life in one execution. */
typedef enum GLTANG_JitFnState {
  GLTANG_JITFN_COLD = 0, ///< Counting polls.
  GLTANG_JITFN_QUEUED,   ///< Over the threshold; the tier-up handler will compile it.
  GLTANG_JITFN_COMPILED, ///< `code` is valid.
  GLTANG_JITFN_NEVER     ///< Failed, declined or discarded: never compiled again.
} GLTANG_JitFnState;

/** @brief One function's feedback and code, in one execution. */
typedef struct GLTANG_JitFn {
  uint32_t polls;
  uint8_t state;       ///< A ::GLTANG_JitFnState.
  uint8_t deopts;
  GRCORE_Code * code;  ///< The cache's reference; compiled code's payload is a ::GLTANG_JitCode.
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
  uint32_t frame_slots;  ///< The function's, so the interpreter slot count is this plus one (the fuel).
  uint32_t local_count;
} GLTANG_JitCode;

/** @brief The execution's JIT state. */
typedef struct GLTANG_Jit {
  GLTANG_JitProgram * programs;  ///< Indexed by the program's index in the execution.
  size_t program_capacity;
  uint64_t * queue;              ///< Function words waiting to be compiled.
  size_t queue_count;
  size_t queue_capacity;
  GRCORE_RequestKind kind;       ///< The tier-up request kind.
  GRCORE_Port * port;            ///< Posts it; taken at the first post.
  const GRJIT_Code * running;    ///< The code being run, for the poll helper; NULL outside compiled code.
  uint64_t last_fword;           ///< The function the last counted poll was in, and its record: a loop polls one function over and over.
  GLTANG_JitFn * last_fn;
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

/** @brief Called by compiled code: charges `n` fuel to the execution, no flush. No GC point. */
void gltang_jit_charge(GLTANG_Execution * exec, uint64_t n);
/** @brief Called by compiled code before a poll: charges `n` and flushes to the context. No GC point. */
void gltang_jit_flush(GLTANG_Execution * exec, uint64_t n);
/**
 * @brief The poll slow path of compiled code, the one GC point (see the file
 *   comment). Returns 0 to continue, 1 after a pause verdict, 2 after an
 *   unwind verdict; the guest frame is current in every case.
 */
uint32_t gltang_jit_poll(void * context, uint64_t function, uint64_t offset);

#endif /* GHOTI_IO_GLTANG_JIT_JIT_INTERNAL_H */
