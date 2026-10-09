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
 * The helpers compiled code calls: the fuel flush and the callee-value note,
 * which are not GC points, and the poll slow path.
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
