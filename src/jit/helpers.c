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
 * `DISCARD`, each the interpreter's own function), the filling of an array or a
 * map literal compiled code has made (the interpreter's own fill), which are not GC
 * points, and the poll slow path.
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

#include <assert.h>

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

// The literals (spec-template-compiled story 4): the container was made by its native, a GC
// point across which the elements stayed in compiled code's registers (in the stack map, so a
// moving collection updated them); they are stored here, after it, by the interpreter's fill.

void gltang_jit_array_fill(GLTANG_Execution * exec, uint64_t array, uint64_t count, uint64_t e0, uint64_t e1, uint64_t e2) {
  if (!gltang_v_is_kind(array, GLTANG_OBJ_ARRAY)) {
    return;
  }
  const GLTANG_Value elements[GLTANG_JIT_FILL_ELEMENTS] = {e0, e1, e2};
  gltang_vm_array_fill(exec, array, elements, (size_t)count);
}

uint64_t gltang_jit_map_fill(GLTANG_Execution * exec, uint64_t map, uint64_t k0, uint64_t v0, uint64_t k1, uint64_t v1) {
  if (!gltang_v_is_kind(map, GLTANG_OBJ_MAP)) {
    return map;  // the allocation's failure, or an earlier call's refused key: the literal's value stays it
  }
  // A second key of 0 means one pair: no key is 0, because a map literal's keys are always string
  // constants (the grammar's, compile.c), or the out-of-memory error where the memory budget refused
  // to make one (gltang_vm_map_fill then makes the literal that error), which the asserts hold the compiler to.
  assert((gltang_v_is_kind(k0, GLTANG_OBJ_STRING) || k0 == exec->roots[GLTANG_ROOT_OOM]) && "a map literal's key is a string");
  assert((!k1 || gltang_v_is_kind(k1, GLTANG_OBJ_STRING) || k1 == exec->roots[GLTANG_ROOT_OOM]) && "a map literal's key is a string");
  const GLTANG_Value pairs[2u * GLTANG_JIT_FILL_PAIRS] = {k0, v0, k1, v1};
  return gltang_vm_map_fill(exec, map, pairs, k1 ? 2u : 1u);
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
