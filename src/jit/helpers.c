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
 * The helpers compiled code calls: the fuel flush, the callee-value notes and
 * the statement-result and discard operations (`CLEAR_RESULT`, `SET_RESULT`,
 * `DISCARD`, each the interpreter's own function), which are not GC points, and
 * the poll slow path.
 *
 * The poll helper does not touch the guest frame. The polling frame is paired
 * with its guest frame by the walk (runtime-core, `a/compiled.h`); the abstract
 * frame reads the compiled frame through its site, and the collector updates
 * the compiled frame's references through its stack map and does not scan the
 * guest frame. So copying the guest frame's slots into the compiled frame after
 * a poll that continued would put stale references over updated ones, and
 * writing the compiled frame's slots into the guest frame is not needed: a
 * handler that wants a slot written is a deoptimizing one (AD-27), and none of
 * ours is. The helper finds nothing through its own frame pointer either: the
 * callable function's poll stub stores the walk start before calling it.
 */

#include <ghoti.io/lang-tang/macros.h>

#include "jit_internal.h"

void gltang_jit_flush(GLTANG_Execution * exec) {
  gltang_vm_flush_fuel(exec);
}

void gltang_jit_note_callee_guard(GLTANG_Execution * exec) {
  exec->jit->refusal = GLTANG_REFUSAL_CALLEE_GUARD;
}

void gltang_jit_note_native_guard(GLTANG_Execution * exec) {
  exec->jit->refusal = GLTANG_REFUSAL_NATIVE_GUARD;
}

void gltang_jit_note_operand_guard(GLTANG_Execution * exec) {
  exec->jit->refusal = GLTANG_REFUSAL_OPERAND_GUARD;
}

void gltang_jit_clear_result(GLTANG_Execution * exec) {
  ++exec->jit_stats.result_clears;
  gltang_vm_set_result(exec, GLTANG_V_NULL, false);
}

void gltang_jit_set_result(GLTANG_Execution * exec, uint64_t value, uint64_t listed) {
  gltang_vm_set_result(exec, value, listed != 0);
}

void gltang_jit_discard(GLTANG_Execution * exec, uint64_t value, uint64_t function, uint64_t offset) {
  // The interpreter's DISCARD: an error is swallowed, and only then is the position
  // recorded (its SYNC is inside the same test), so a value that is not an error
  // leaves `current_function` and `current_offset` as the interpreter leaves them.
  if (gltang_vm_is_error(value)) {
    exec->current_function = (uint32_t)function;
    exec->current_offset = (uint32_t)offset;
    gltang_vm_error_swallowed(exec, GLTANG_ERROR_HOW_DISCARDED, value, exec->act);
  }
}

uint64_t gltang_jit_callee_is_native(uint64_t callee, uint64_t member) {
  if (!gltang_v_is_kind(callee, GLTANG_OBJ_NATIVE)) {
    return 0;
  }
  const GLTANG_NativeObject * native = gltang_object(callee);
  return native->member == (const GLTANG_LibraryMember *)(uintptr_t)member && native->builtin == (uint32_t)GLTANG_BUILTIN_NONE;
}

uint32_t gltang_jit_poll(void * ctx, uint64_t fword, uint64_t index) {
  GRCORE_Context * context = ctx;
  GLTANG_Execution * exec = gltang_vm_execution_of(context);
  if (!exec || !exec->jit) {
    return 2u;
  }
  ++exec->jit_stats.slow_polls;
  // What the interpreter's SYNC() does: where the poll is, for locations and for
  // errors made next.
  exec->current_function = GLTANG_FN_INDEX(fword);
  exec->current_offset = (uint32_t)index;
  GRCORE_Verdict verdict = grcore_stack_poll(context, fword, index);
  if (verdict == GRCORE_VERDICT_PAUSE) {
    return 1u;
  }
  if (verdict == GRCORE_VERDICT_UNWIND) {
    return 2u;
  }
  return 0u;
}
