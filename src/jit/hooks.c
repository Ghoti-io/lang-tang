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
 * lang-tang's side of the call protocol (AD-28): the four hooks runtime-jit's
 * compiled code calls around a call to another compiled guest function, over
 * runtime-core's stack, registry and rebuild.
 *
 * - `push` makes the callee's guest frame exactly as the interpreter's `CALL`
 *   does (the same depth test, the same push, the same header, the arguments in
 *   the first locals), and adds the `CALL`'s fuel after the push has succeeded,
 *   so a refused push charges nothing and the interpreter charges the call when
 *   it makes it.
 * - `pop` takes the callee's frame off.
 * - `compile` compiles a callee at its first call from compiled code.
 * - `deopt` rebuilds every compiled frame of the chain into its guest frame.
 *
 * Every hook is handed the context, and finds the execution from it. None of
 * them calls the interpreter. Only `push` can collect (the frame push is a GC
 * point); `pop` and `compile` do not, and `deopt` allocates nothing.
 */

#include <ghoti.io/lang-tang/macros.h>

#include "jit_internal.h"

#include <ghoti.io/runtime-heap/heap.h>

#include <stdint.h>

#define H GLTANG_FRAME_HEADER

static inline GLTANG_Execution * exec_of(void * context) {
  return gltang_vm_execution_of(context);
}

/** The function a call token names, or NULL for a token that names none. */
static const GLTANG_Function * function_of(const GLTANG_Execution * exec, uint64_t fword) {
  const GLTANG_Program * program = gltang_exec_program(exec, GLTANG_FN_PROGRAM(fword));
  return program && GLTANG_FN_INDEX(fword) < program->function_count ? &program->functions[GLTANG_FN_INDEX(fword)] : NULL;
}

static uint32_t refuse_push(GLTANG_Jit * jit) {
  jit->refusal = GLTANG_REFUSAL_PUSH;
  return 1u;
}

static uint32_t hook_push(void * context, uint64_t callee, const uint64_t * args, uint64_t count) {
  GLTANG_Execution * exec = exec_of(context);
  GLTANG_Jit * jit = exec ? exec->jit : NULL;
  if (!jit) {
    return 1u;
  }
  // The hook checks its own arguments: the token must decode to a function of a
  // loaded program, the count must be that function's parameters and the hidden
  // flag, and the flag must be 0 (a compiled call). Anything else is a refusal,
  // counted, which no correct run has.
  const GLTANG_Function * cf = function_of(exec, callee);
  if (!cf || count != (uint64_t)cf->parameter_count + 1u || args[count - 1u] != 0u) {
    ++exec->jit_stats.hook_argument_errors;
    return refuse_push(jit);
  }
  // The interpreter's order: the guest-depth test first, then the extension of
  // the reservation (by zero: lang-tang has no converting location), then the
  // push. The push is the frame-push GC point. It does not fire the memory
  // budget's CALL_REFUSED poll: the interpreter makes the call after the exit
  // and fires it once.
  if (grcore_context_depth(context, GRCORE_DEPTH_GUEST) >= grcore_context_guest_depth(context)) {
    return refuse_push(jit);
  }
  GRCORE_Stack * stack = grcore_context_stack(context);
  GRCORE_FrameRef frame;
  if (grcore_stack_push(stack, exec->engine, cf->frame_slots, &frame) != GRCORE_OK) {
    return refuse_push(jit);
  }
  // The frame push is a GC point (AD-28), and the heap's torture mode owns only
  // the ones the heap has, so under torture the engine makes this one: a
  // collection here (and, with relocation, a move) must find every argument in the
  // call site's stack map and update it in place. A collection reads every frame,
  // so one at each push of a recursion thousands deep would cost the square of its
  // depth: the first two hundred and fifty-six levels of the guest stack collect at
  // every push, and below them every thousand and twenty-fourth push does.
  if (jit->gc_at_push && (grcore_context_depth(context, GRCORE_DEPTH_GUEST) <= 256u || (exec->jit_stats.calls & 1023u) == 0)) {
    (void)grheap_collect(exec->heap);
  }
  // The arguments are read after the push, and after any collection it caused:
  // the push may have moved the guest stack, and a collection updated the
  // argument words in place.
  uint64_t * S = grcore_stack_slots(stack, frame);
  S[GLTANG_F_FUNCTION] = callee;
  S[GLTANG_F_PC] = 0;
  S[GLTANG_F_SP] = H + cf->local_count;
  S[GLTANG_F_FLAGS] = exec->act->depth;
  const uint32_t parameters = cf->parameter_count;
  for (uint32_t i = 0; i < parameters; ++i) {
    S[H + i] = args[i];
  }
  for (uint32_t i = parameters; i < cf->local_count; ++i) {
    S[H + i] = 0;
  }
  (void)grcore_stack_set_identity(stack, frame, (GRCORE_PollIdentity){callee, 0});
  // The CALL's own fuel, now that the call is made.
  exec->pending_fuel += gltang_opcode_cost_table[GLTANG_OP_CALL];
  ++exec->jit_stats.calls;
  if (++jit->chain > exec->jit_stats.deepest_chain) {
    exec->jit_stats.deepest_chain = jit->chain;
  }
  return 0u;
}

static void hook_pop(void * context) {
  GLTANG_Execution * exec = exec_of(context);
  if (!exec) {
    return;
  }
  (void)grcore_stack_pop(grcore_context_stack(context));
  if (exec->jit && exec->jit->chain) {
    --exec->jit->chain;
  }
}

static uint32_t hook_compile(void * context, uint64_t callee) {
  GLTANG_Execution * exec = exec_of(context);
  GLTANG_Jit * jit = exec ? exec->jit : NULL;
  if (!jit) {
    return 1u;
  }
  ++exec->jit_stats.compile_hook_calls;
  GLTANG_JitFn * f = gltang_jit_find_fn(exec, jit, callee, true);
  if (!f) {
    jit->refusal = GLTANG_REFUSAL_REMEMBERED;
    return 1u;
  }
  if (f->state == GLTANG_JITFN_NEVER) {
    // Cannot happen (the slot mirrors the state), but a slot that was somehow empty
    // for a function that can never be compiled is made right here.
    if (f->slot) {
      (void)grcore_entry_slot_refuse(exec->context, f->slot);
    }
    jit->refusal = GLTANG_REFUSAL_REMEMBERED;
    return 1u;
  }
  if (!gltang_jit_compile_function(exec, callee)) {
    // Declined or failed: the function is never compiled and its slot is refused
    // (compile_function did both), so later calls cost one compare and no hook.
    jit->refusal = GLTANG_REFUSAL_REMEMBERED;
    return 1u;
  }
  ++exec->jit_stats.compile_at_call;
  return 0u;
}

static bool call_site_at(const GLTANG_JitCode * jc, uint64_t offset) {
  return jc && offset < jc->code_count && jc->call_sites && (jc->call_sites[offset >> 3] & (1u << (offset & 7u))) != 0;
}

static uint32_t hook_deopt(void * context, uint64_t cause) {
  GLTANG_Execution * exec = exec_of(context);
  GLTANG_Jit * jit = exec ? exec->jit : NULL;
  if (!jit) {
    return (uint32_t)GRCORE_ERR_INVALID;
  }
  GRCORE_Stack * stack = grcore_context_stack(context);
  const size_t frame_count = grcore_stack_frame_count(stack);
  size_t keep = SIZE_MAX;
  // Only a guard or an unconditional exit (cause 0) counts toward a function's
  // discard limit; a poll's deopt (a pause, cause 1, or an unwind, cause 2) does
  // not: a debugger step or a fuel pause must not throw away hot code.
  bool counts = cause == 0u;
  uint64_t innermost_fword = 0;
  bool any = false;

  // The walk, before the rebuild (which clears the record's compiled state): which
  // frame is innermost and why we left it, and the identity of every caller's
  // guest frame, which the interpreter sets at a call (`grcore_stack_set_identity`)
  // and the rebuild does not.
  GRCORE_CompiledWalk walk;
  GRCORE_CompiledFrame fr;
  if (grcore_compiled_walk_begin(context, &walk) == GRCORE_OK) {
    GRCORE_FrameRef g = grcore_stack_top(stack);
    size_t g_idx = frame_count ? frame_count - 1u : 0;
    size_t record = 0;
    while (grcore_compiled_walk_next(&walk, &fr) == GRCORE_CWALK_FRAME) {
      if (any && fr.record != record) {
        break;  // a run under a nested activation: the rebuild below does not cover it
      }
      const size_t idx = fr.base_frames - 1u + (fr.run_length - 1u - fr.run_depth);
      if (!any) {
        any = true;
        record = fr.record;
        innermost_fword = fr.identity.function;
        if (cause == 2u) {
          // An unwind pops frames without converting them: every compiled frame of
          // this run is inside the scope that is unwinding (compiled code opens
          // none), so the rebuild skips them all and only marks the record rebuilt.
          keep = fr.base_frames - 1u;
        }
        if (cause == 0u) {
          GLTANG_JitFn * jf = gltang_jit_find_fn(exec, jit, fr.identity.function, false);
          const GLTANG_JitCode * jc = jf && jf->code ? grcore_code_payload(jf->code) : NULL;
          if (call_site_at(jc, fr.identity.offset)) {
            // An exit at a call site. Whoever refused recorded why, and none of
            // these counts toward a discard limit except the callee-value guard.
            counts = false;
            if (idx + 1u < frame_count) {
              ++exec->jit_stats.call_exits_native_stack;
            }
            else if (jit->refusal == GLTANG_REFUSAL_CALLEE_GUARD) {
              ++exec->jit_stats.call_exits_callee_guard;
              counts = true;
            }
            else if (jit->refusal == GLTANG_REFUSAL_PUSH) {
              ++exec->jit_stats.call_exits_push_refused;
            }
            else {
              ++exec->jit_stats.call_exits_remembered;
            }
          }
        }
      }
      while (g_idx > idx) {
        g = grcore_stack_caller(stack, g);
        --g_idx;
      }
      if (idx + 1u < frame_count) {
        (void)grcore_stack_set_identity(stack, g, fr.identity);
      }
    }
  }
  jit->refusal = GLTANG_REFUSAL_NONE;
  jit->chain = 0;

  uint32_t rc;
  if (jit->test_fail_rebuild) {
    rc = (uint32_t)GRCORE_ERR_INVALID;
  }
  else {
    size_t rebuilt = 0;
    GRCORE_Result r = grcore_compiled_rebuild(context, jit->reservation, keep, &rebuilt);
    rc = r == GRCORE_OK ? 0u : (uint32_t)r;
  }
  if (rc == 0u && any) {
    jit->exit_fword = innermost_fword;
    jit->exit_counted = counts;
    jit->exit_valid = true;
  }
  // The compiled calls are gone, so are their extensions of the reservation (none
  // here: a call extends it by zero).
  return rc;
}

const GRJIT_CallHooks gltang_jit_hooks = {
  .push = hook_push,
  .pop = hook_pop,
  .compile = hook_compile,
  .deopt = hook_deopt,
  .tail = NULL,
};
