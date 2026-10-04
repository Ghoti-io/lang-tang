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
 * The three helpers compiled code calls: the two fuel helpers, which are not
 * GC points, and the poll slow path, which is the only one.
 *
 * This file is built with frame pointers (the Makefile's JIT_MODULE_CFLAGS,
 * stamped), because `gltang_jit_poll` finds the compiled frame through its own
 * frame-pointer chain: `__builtin_frame_address(0)` is this function's `rbp`,
 * the word it points at is the saved `rbp` of the compiled function that called
 * (its frame base, which `a/deopt.h` reads slots from), and the word after it is
 * the return address, which is the site's code offset after the code's base is
 * taken off. The read-back test of runtime-jit does the same.
 */

#include <ghoti.io/lang-tang/macros.h>

#include "jit_internal.h"

#define H GLTANG_FRAME_HEADER

void gltang_jit_charge(GLTANG_Execution * exec, uint64_t n) {
  exec->pending_fuel += n;
}

void gltang_jit_flush(GLTANG_Execution * exec, uint64_t n) {
  exec->pending_fuel += n;
  gltang_vm_flush_fuel(exec);
}

__attribute__((noinline)) uint32_t gltang_jit_poll(void * ctx, uint64_t fword, uint64_t index) {
  const uintptr_t * fp = (const uintptr_t *)__builtin_frame_address(0);
  uintptr_t frame_base = fp[0];
  uintptr_t return_address = fp[1];
  GRCORE_Context * context = ctx;
  GLTANG_Execution * exec = gltang_vm_execution_of(context);
  if (!exec || !exec->jit || !exec->jit->running) {
    return 2u;
  }
  ++exec->jit_stats.slow_polls;
  const GRJIT_Code * code = exec->jit->running;
  uintptr_t base = (uintptr_t)grjit_code_address(code);
  const GRCORE_CodeSite * site = return_address >= base
    ? grcore_codemeta_find(grjit_code_meta(code), (uint32_t)(return_address - base))
    : NULL;
  const GLTANG_Program * program = gltang_exec_program(exec, GLTANG_FN_PROGRAM(fword));
  if (!site || !program || GLTANG_FN_INDEX(fword) >= program->function_count) {
    return 2u;
  }
  const GLTANG_Function * fn = &program->functions[GLTANG_FN_INDEX(fword)];
  size_t n = (size_t)fn->frame_slots + 1u;
  uint64_t slots[GLTANG_JIT_MAX_SLOTS];
  if (n > GLTANG_JIT_MAX_SLOTS || grcore_deopt_read(site, (const void *)frame_base, slots, n) != GRCORE_OK) {
    return 2u;
  }

  // Make the guest frame current: what the interpreter's own SAVE() leaves for
  // its own poll (pc after the poll, sp as it is here), the locals and the
  // operand stack. The header's function and flags words are the frame's own.
  GRCORE_Stack * stack = grcore_context_stack(context);
  GRCORE_FrameRef frame = grcore_stack_top(stack);
  uint64_t * S = grcore_stack_slots(stack, frame);
  S[GLTANG_F_PC] = slots[GLTANG_F_PC];
  S[GLTANG_F_SP] = slots[GLTANG_F_SP];
  for (size_t k = H; k < fn->frame_slots; ++k) {
    S[k] = slots[k];
  }
  // What the interpreter's SYNC() does: where the poll is, for locations and
  // for errors made next.
  exec->current_function = GLTANG_FN_INDEX(fword);
  exec->current_offset = (uint32_t)index;

  GRCORE_Verdict verdict = grcore_stack_poll(context, fword, index);

  if (verdict == GRCORE_VERDICT_PAUSE) {
    return 1u;
  }
  if (verdict == GRCORE_VERDICT_UNWIND) {
    return 2u;
  }
  // Continue. The guest stack may have moved (GLTANG_TEST_MOVING_STACK is a
  // test mode for exactly this) and a collector may have updated a reference
  // in a slot in place, so the frame is read afresh and its slots are copied
  // back into the compiled frame, through the same metadata that read them.
  S = grcore_stack_slots(stack, frame);
  for (size_t k = H; k < fn->frame_slots; ++k) {
    slots[k] = S[k];
  }
  if (grcore_deopt_write_back(site, (void *)frame_base, slots, n) != GRCORE_OK) {
    return 2u;
  }
  return 0u;
}
