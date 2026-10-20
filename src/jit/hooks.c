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
 * lang-tang's side of the call protocol (AD-28): the hooks runtime-jit's
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
 * point, and so is the top-up of the float-box pool that comes before it); `pop`
 * and `compile` do not, and `deopt` allocates nothing: the only boxing there is
 * is the rebuild's, from boxes made ahead of time (AD-27).
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

/** The record of a function, by its token, without the checks `gltang_jit_find_fn` makes for a caller that may create it. */
static inline const GLTANG_JitFn * record_of(GLTANG_Execution * exec, const GLTANG_Jit * jit, uint64_t fword) {
  ++exec->jit_stats.record_lookups;
  const uint32_t p = GLTANG_FN_PROGRAM(fword);
  const uint32_t f = GLTANG_FN_INDEX(fword);
  if (p >= jit->program_capacity) {
    return NULL;
  }
  const GLTANG_JitProgram * program = &jit->programs[p];
  return program->fns && f < program->count ? &program->fns[f] : NULL;
}

/** The context's guest stack: a context's is made when the engine registers and is never replaced, so the first hook to need it keeps it (for that context: a hook handed another one looks again). */
static inline GRCORE_Stack * stack_of(GLTANG_Jit * jit, void * context) {
  return gltang_jit_stack_of(jit, context);
}

static uint32_t refuse_push(GLTANG_Jit * jit) {
  jit->refusal = GLTANG_REFUSAL_PUSH;
  return 1u;
}

static uint32_t hook_push(void * context, void * callee_environment, uint64_t callee, const uint64_t * args, uint64_t count, uint64_t result_count) {
  GLTANG_Execution * exec = exec_of(context);
  GLTANG_Jit * jit = exec ? exec->jit : NULL;
  if (!jit) {
    return 1u;
  }
  // The hook checks its own arguments: the token must decode to a function of a
  // loaded program, the count must be that function's parameters and the hidden
  // flag, and the flag must be 0 (a compiled call). The call's environment must be
  // this execution (Tang's guest functions share their owning execution as their
  // language environment; the operand is explicit in the JIT ABI so that another
  // embedder can name a different one) and its result shape Tang's one. Anything
  // else is a refusal, counted, which no correct run has.
  const GLTANG_Function * cf = function_of(exec, callee);
  if (!cf || callee_environment != exec || result_count != 1u ||
      count != (uint64_t)cf->parameter_count + 1u || !args || args[count - 1u] != 0u) {
    ++exec->jit_stats.hook_argument_errors;
    return refuse_push(jit);
  }
  // The interpreter's order: the guest-depth test first, then the extension of
  // the reservation by the callee's most raw floats at one site (zero for a
  // function that holds none, which costs nothing) with the top-up of the pool
  // behind it, then the push. A refusal of the extension is a refused push: the
  // call site exits and the interpreter makes the call. The top-up allocates, so
  // it is before the arguments are read (a collection updates them in place); the
  // push is the frame-push GC point. Neither fires the memory budget's
  // CALL_REFUSED poll: the interpreter makes the call after the exit and fires it
  // once.
  if (grcore_context_depth(context, GRCORE_DEPTH_GUEST) >= grcore_context_guest_depth(context)) {
    return refuse_push(jit);
  }
  // The callee's most raw floats at one site, from the function's record. This is the
  // code the call goes on to run: a compiled call reaches this hook through the
  // callee's entry slot, whose code is installed together with the record's `code`
  // (`gltang_jit_compile_function`: the install is refused for another function's
  // code, and `discard` refuses the slot in the same step that clears `code`), and
  // nothing changes either between the slot's check and this hook (one thread, no
  // poll in between). A record that is not compiled, which only the test hook can
  // call, extends by zero, and a rebuild that then needed more is refused up front by
  // the core (and counted), never made short.
  //
  // A call whose callee holds no raw float, which is every call until some compiled
  // function does, pays one load and one compare here and in the pop hook: no lookup,
  // no record, no pool work (the statistics `extension_lookups` and `extension_records`
  // stay zero, which a test holds it to). `max_converting` of a record is nonzero only
  // while it is compiled.
  size_t extension = 0;
  if (jit->floaty) {
    ++exec->jit_stats.extension_lookups;
    const GLTANG_JitFn * callee_fn = record_of(exec, jit, callee);
    extension = callee_fn ? callee_fn->max_converting : 0;
    if (extension != 0 && !gltang_jit_extend(exec, extension, jit->chain + 1u, callee)) {
      return refuse_push(jit);
    }
  }
  GRCORE_Stack * stack = stack_of(jit, context);
  GRCORE_FrameRef frame;
  const bool injected = extension != 0 && jit->test_fail_stack_push_at != 0 && ++jit->test_stack_pushes == jit->test_fail_stack_push_at;
  if (injected || grcore_stack_push(stack, exec->engine, cf->frame_slots, &frame) != GRCORE_OK) {
    if (extension != 0) {
      gltang_jit_retract_level(exec, jit->chain + 1u, callee);
    }
    if (injected) {
      jit->test_failed_capacity = grcore_deopt_reservation_capacity(jit->reservation);
      jit->test_failed_extensions = jit->ext_count;
      ++jit->test_failed_count;
    }
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
  (void)grcore_stack_set_identity(stack, frame,
      (GRCORE_PollIdentity){callee, 0, exec->engine});
  // The CALL's own fuel, now that the call is made.
  exec->pending_fuel += gltang_opcode_cost_table[GLTANG_OP_CALL];
  ++exec->jit_stats.calls;
  if (++jit->chain > exec->jit_stats.deepest_chain) {
    exec->jit_stats.deepest_chain = jit->chain;
  }
  return 0u;
}

/** Tang currently has one guest result per function. Tail calls remain an
 * interpreter fallback, but validate the same boundary contract before
 * declining them so a mismatched result shape can never replace a guest frame. */
static uint32_t hook_tail(void * context, void * callee_environment, uint64_t callee, const uint64_t * args, uint64_t count, uint64_t result_count) {
  GLTANG_Execution * exec = exec_of(context);
  GLTANG_Jit * jit = exec ? exec->jit : NULL;
  if (!jit) {
    return 1u;
  }
  const GLTANG_Function * cf = function_of(exec, callee);
  if (callee_environment != exec || !cf || result_count != 1u ||
      count != (uint64_t)cf->parameter_count + 1u || !args || args[count - 1u] != 0u) {
    ++exec->jit_stats.hook_argument_errors;
  }
  // Tang does not yet replace guest frames for a compiled tail call. Refusing
  // leaves the stack untouched and lets the call site's interpreter fallback run.
  return refuse_push(jit);
}

static void hook_pop(void * context) {
  GLTANG_Execution * exec = exec_of(context);
  if (!exec) {
    return;
  }
  GLTANG_Jit * jit = exec->jit;
  GRCORE_Stack * stack = jit ? stack_of(jit, context) : grcore_context_stack(context);
  // The extension this pop gives back is the innermost one if it was made at this depth
  // (at most one call is open at a depth, so a record at this depth is this call's, and a
  // call that extended by zero left none). The test is on the records alone: a call with no
  // raw float pays one compare here, and the stack is read only when there is a record to
  // give back, to check that it is this callee's. A record of another function at this
  // depth is not given back and is counted: giving it back would leave a later call
  // without its share, and leaving it makes the entry's exit (which gives back all) the
  // one to clean up. The guard on the frame count is only here, where a frame is read.
  if (jit && jit->ext_count > jit->ext_floor && jit->ext[jit->ext_count - 1u].level == jit->chain) {
    const uint64_t callee = grcore_stack_frame_count(stack) ? grcore_stack_slots(stack, grcore_stack_top(stack))[GLTANG_F_FUNCTION] : 0;
    if (jit->ext[jit->ext_count - 1u].fword == callee) {
      gltang_jit_retract_level(exec, jit->chain, callee);
    }
    else {
      ++exec->jit_stats.extension_mismatches;
    }
  }
  (void)grcore_stack_pop(stack);
  if (jit && jit->chain) {
    --jit->chain;
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

static bool expected_site_at(const GLTANG_JitCode * jc, uint64_t offset) {
  return jc && offset < jc->code_count && jc->expected_sites && (jc->expected_sites[offset >> 3] & (1u << (offset & 7u))) != 0;
}

static bool native_site_at(const GLTANG_JitCode * jc, uint64_t offset) {
  return jc && offset < jc->code_count && jc->native_sites && (jc->native_sites[offset >> 3] & (1u << (offset & 7u))) != 0;
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
  bool expected = false;  // the exit is a designed one (a raw float boxed for its consumer)
  // A native's status leaves compiled code with `GRJIT_CAUSE_NATIVE | status`: the
  // unwind status is a poll's unwind (every compiled frame of the run is inside
  // the scope that is unwinding), any other is a leave, with the state after the call.
  const bool native_cause = (cause & GRJIT_CAUSE_NATIVE) != 0u;
  const uint32_t native_status = (uint32_t)cause;
  const bool unwinding = cause == 2u || (native_cause && native_status == GRJIT_NATIVE_UNWIND);
  uint64_t innermost_fword = 0;
  bool any = false;

  // The walk, before the rebuild (which clears the record's compiled state): which
  // frame is innermost and why we left it. The rebuild gives every rebuilt guest
  // frame its compiled frame's identity, so the hook does not.
  GRCORE_CompiledWalk walk;
  GRCORE_CompiledFrame fr;
  if (grcore_compiled_walk_begin(context, &walk) == GRCORE_OK) {
    if (grcore_compiled_walk_next(&walk, &fr) == GRCORE_CWALK_FRAME) {
      const size_t idx = fr.base_frames - 1u + (fr.run_length - 1u - fr.run_depth);
      any = true;
      innermost_fword = fr.identity.function;
      if (native_cause) {
        if (native_status == GRJIT_NATIVE_UNWIND) {
          ++exec->jit_stats.native_status_unwinds;
        }
        else {
          ++exec->jit_stats.native_status_deopts;
        }
      }
      if (unwinding) {
        // An unwind pops frames without converting them: every compiled frame of
        // this run is inside the scope that is unwinding (compiled code opens
        // none), so the rebuild skips them all and only marks the record rebuilt.
        keep = fr.base_frames - 1u;
      }
      if (cause == 0u) {
        // The sites are those of the code that ran, found by the address the frame stopped
        // at (the registry keeps a retired range's handle for as long as a frame can return
        // into it), not the function's record, whose code is gone if the function was
        // discarded while this record was open (a nested run's eighth exit).
        GRCORE_CodeRange ran;
        const GLTANG_JitCode * jc = grcore_code_lookup(context, (uintptr_t)fr.return_address, &ran) && ran.code ? grcore_code_payload(ran.code) : NULL;
        if (expected_site_at(jc, fr.identity.offset)) {
          // The designed exit: a raw float is boxed for its consumer. Not a failure of compiled
          // code, so it does not count toward the discard limit (and is counted, with the
          // others, by the entry once the rebuild has succeeded).
          expected = true;
          counts = false;
        }
        else if (native_site_at(jc, fr.identity.offset)) {
          // An exit at a library call or member load: the callee value was not the
          // native the site names (counts, like the guest call's), or the native
          // stack would have run out before the call (does not).
          if (jit->refusal == GLTANG_REFUSAL_NATIVE_GUARD) {
            ++exec->jit_stats.native_call_exits_guard;
            counts = true;
          }
          else {
            ++exec->jit_stats.native_exits_stack;
            counts = false;
          }
        }
        else if (call_site_at(jc, fr.identity.offset)) {
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
  }
  jit->refusal = GLTANG_REFUSAL_NONE;
  jit->chain = 0;

  uint32_t rc;
  if (jit->test_fail_rebuild) {
    rc = (uint32_t)GRCORE_ERR_INVALID;
    // What the failed rebuild left in the guest frames: a value slot whose word looks
    // like an object and is not one is a raw word where a value belongs. (A collection
    // would not see it: the guest frames of a run that was never rebuilt are paired with
    // their compiled frames and are not scanned.)
    {
      GRCORE_FrameRef frame = grcore_stack_top(stack);
      for (size_t f = 0; f < frame_count; ++f) {
        size_t slots = 0;
        if (grcore_stack_slot_count(stack, frame, &slots) == GRCORE_OK) {
          const uint64_t * words = grcore_stack_slots(stack, frame);
          for (size_t w = H; w < slots; ++w) {
            if (gltang_v_is_object(words[w]) && !grheap_contains(exec->heap, (const void *)(uintptr_t)words[w])) {
              ++jit->test_stray_words;
            }
          }
        }
        frame = grcore_stack_caller(stack, frame);
      }
    }
    if (jit->gc_at_push) {
      // Under torture the unwind that follows collects at its first allocation, with the
      // guest frames as the failed rebuild left them: they must hold no raw word.
      (void)grheap_collect(exec->heap);
    }
  }
  else {
    size_t rebuilt = 0;
    GRCORE_Result r = grcore_compiled_rebuild(context, jit->reservation, keep, &rebuilt);
    rc = r == GRCORE_OK ? 0u : (uint32_t)r;
  }
  if (rc == 0u && any) {
    jit->exit_fword = innermost_fword;
    jit->exit_counted = counts;
    jit->exit_expected = expected;
    jit->exit_valid = true;
  }
  // The compiled calls are gone, so are their extensions of the reservation. The
  // entry's own stays until the entry gives it back; the rebuild above needed all
  // of them, so none was retracted before it.
  gltang_jit_retract_to(exec, jit->ext_floor);
  return rc;
}

const GRJIT_CallHooks gltang_jit_hooks = {
  .push = hook_push,
  .pop = hook_pop,
  .compile = hook_compile,
  .deopt = hook_deopt,
  .tail = hook_tail,
};
