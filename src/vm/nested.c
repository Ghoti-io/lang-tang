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
 * A native that re-enters guest code (AD-23): the nested activation.
 *
 * The stack's own builtins that call guest code are either self-hosted or
 * resumable; a host function is opaque and may not. What this file offers is the
 * third thing the architecture names for the stack's own natives, a nested
 * activation: the native opens a `REENTRY` record (nested, so a pause verdict
 * inside it becomes an unwind that stops at the record, AD-5), pushes the
 * function's guest frame, and runs the interpreter loop on it until that frame
 * returns. The loop is the one the program runs in, with a base to return at, and
 * it enters compiled code as it does for any function: the compiled frames below
 * the native are described by the record the native's call opened, the nested ones
 * by a record of their own. No public API reaches this; the engine's test natives
 * (testnatives.c) are its only users.
 */

#include <ghoti.io/lang-tang/macros.h>

#include <string.h>
#include <ghoti.io/runtime-core/a/activation.h>
#include <ghoti.io/runtime-heap/heap.h>
#include "vm_internal.h"

#define H GLTANG_FRAME_HEADER
#define COPY_MAX 16u

GLTANG_Value gltang_vm_call_guest(GLTANG_Execution * exec, GLTANG_Value function, size_t argc, const GLTANG_Value * args) {
  GRCORE_Context * context = exec->context;
  GRCORE_Stack * stack = grcore_context_stack(context);
  if (!gltang_v_is_function(function) || argc > COPY_MAX) {
    return gltang_vm_make_error(exec, GLTANG_ERROR_INVALID_FUNCTION_CALL);
  }
  const GLTANG_Program * program = gltang_exec_current_program(exec);
  uint64_t target = gltang_v_function_index(function);
  if (target >= program->function_count) {
    return gltang_vm_make_error(exec, GLTANG_ERROR_INVALID_FUNCTION_CALL);
  }
  const GLTANG_Function * callee = &program->functions[target];
  if (callee->parameter_count != argc) {
    return gltang_vm_make_error(exec, GLTANG_ERROR_ARGUMENT_COUNT_MISMATCH);
  }
  if (grcore_context_depth(context, GRCORE_DEPTH_GUEST) >= grcore_context_guest_depth(context)) {
    return gltang_vm_make_error(exec, GLTANG_ERROR_RECURSION_LIMIT);
  }
  // The arguments are copied into this frame before the record is open and before
  // anything can collect; the record gives this frame as its conservative segment,
  // so what they name is pinned.
  GLTANG_Value held[COPY_MAX + 1u];
  if (argc) {
    memcpy(held, args, argc * sizeof(GLTANG_Value));
  }
  const uintptr_t frame_address = (uintptr_t)__builtin_frame_address(0);
  GRCORE_CSegment segment = {gltang_vm_segment_low(frame_address, &held[0]), frame_address + 16u};
  GRCORE_ActivationRef record;
  GRCORE_Result entered = grcore_activation_enter(stack, GRCORE_ACTIVATION_REENTRY, exec->engine, true, &segment, &record);
  if (entered != GRCORE_OK) {
    return gltang_vm_native_refused_value(exec, entered);
  }
  if (exec->native_gc_seam) {
    // Entering a nested activation is a GC point (AD-17).
    (void)grheap_collect(exec->heap);
  }
  // The frame under the native is stopped at the call: its identity is where the
  // call was, as the interpreter's own calls leave it.
  GRCORE_FrameRef caller = grcore_stack_top(stack);
  const size_t base = grcore_stack_frame_count(stack);
  if (caller.offset) {
    const uint64_t * slots = grcore_stack_slots(stack, caller);
    (void)grcore_stack_set_identity(stack, caller, (GRCORE_PollIdentity){slots[GLTANG_F_FUNCTION], exec->current_offset});
  }
  GRCORE_FrameRef frame;
  GRCORE_Result pushed = grcore_stack_push(stack, exec->engine, callee->frame_slots, &frame);
  if (pushed != GRCORE_OK) {
    (void)grcore_activation_leave(stack, record);
    return gltang_vm_native_refused_value(exec, pushed);
  }
  {
    // The push is a GC point and may have moved the stack: the slots are read afresh,
    // and the arguments come from the pinned copies.
    uint64_t * slots = grcore_stack_slots(stack, frame);
    slots[GLTANG_F_FUNCTION] = GLTANG_FN_WORD(exec->act->program_index, target);
    slots[GLTANG_F_PC] = 0;
    slots[GLTANG_F_SP] = H + callee->local_count;
    slots[GLTANG_F_FLAGS] = exec->act->depth;
    for (size_t i = 0; i < argc; ++i) {
      slots[H + i] = held[i];
    }
  }
  const size_t saved_frames = exec->run_base_frames;
  GLTANG_Activation * const saved_act = exec->run_base_act;
  const size_t saved_temps = exec->run_base_temps;
  const GRCORE_ActivationRef saved_record = exec->run_base_record;
  const uint32_t saved_function = exec->current_function;
  const uint32_t saved_offset = exec->current_offset;
  exec->run_base_frames = base;
  exec->run_base_act = exec->act;
  exec->run_base_temps = exec->temp_count;
  exec->run_base_record = record;
  GRCORE_Step step = gltang_vm_run(exec, context);
  exec->run_base_frames = saved_frames;
  exec->run_base_act = saved_act;
  exec->run_base_temps = saved_temps;
  exec->run_base_record = saved_record;
  exec->current_function = saved_function;
  exec->current_offset = saved_offset;
  GLTANG_Value result = step == GRCORE_STEP_FINISHED ? exec->nested_value : GLTANG_V_UNWIND;
  if (step == GRCORE_STEP_PAUSED) {
    // Cannot happen: the record is nested, so a pause verdict was an unwind. If it
    // did, the frames above the record go and the run ends.
    (void)grcore_unwind_to_activation(stack, record, NULL);
    exec->unwinding = true;
  }
  exec->nested_value = 0;
  (void)grcore_activation_leave(stack, record);
  return result;
}
