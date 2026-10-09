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
 * The baseline JIT's policy: what is hot, when to compile, the cache, and the
 * entry into compiled code and the exits from it.
 *
 * Tier-up is a poll handler registered by key (AD-19) in phase ACT (AD-5). The
 * interpreter's `POLL` counts the function's polls; crossing the threshold
 * queues the function and posts a request of this module's own kind, so the
 * poll that follows runs the handler. The handler acts only when the verdict
 * is continue, compiles every queued function synchronously, and clears the
 * request; on a pause or unwind it does nothing and the request stays pending
 * until a poll that continues. It allocates nothing in the GC heap: it uses
 * the context's counting allocator and its counting page provider, so compiled
 * code is on the context's meter (AD-13).
 *
 * Feedback lives with the execution (AD-22): the counters, the queue and the
 * compiled code are the execution's, and compiled code bakes in the
 * execution's own addresses. Nothing is shared between contexts and there is
 * no compiler thread, which design.md records as not done.
 *
 * Every compiled function is registered with the context (`grcore_code_register`)
 * while it is installed, and has one entry slot (`GRCORE_EntrySlot`) that the
 * context owns, made when the function is first named and kept for the
 * execution. The slot's state mirrors the function's: empty while it is cold,
 * code once it is compiled, refused once it can never be (declined, failed or
 * discarded), so a call site that finds a refused slot exits at the cost of one
 * compare. Code is never destroyed directly: a function that is discarded
 * refuses its slot, unregisters its range and drops the cache's reference, and
 * the registry frees it once no JIT record is open, so a frame waiting in a
 * chain still returns into it.
 */

#include <ghoti.io/lang-tang/macros.h>

#include "jit_internal.h"

#include <string.h>
#include <ghoti.io/cutil/memory.h>

#define H GLTANG_FRAME_HEADER

// ---------------------------------------------------------------------------
// The tier-up handler
// ---------------------------------------------------------------------------

static void tierup_poll(GRCORE_Context * context, void * value, GRCORE_PollCall * call);

static const GRCORE_Key tierup_key = GRCORE_KEY_INIT(
  .name = "lang-tang tier-up",
  .cardinality = GRCORE_CARDINALITY_ONE,
  .phase = GRCORE_PHASE_ACT,
  .destroy = NULL,
  .poll = tierup_poll,
);

GLTANG_JitFn * gltang_jit_find_fn(GLTANG_Execution * exec, GLTANG_Jit * jit, uint64_t fword, bool create) {
  uint32_t p = GLTANG_FN_PROGRAM(fword);
  uint32_t f = GLTANG_FN_INDEX(fword);
  if (p >= exec->program_count) {
    return NULL;
  }
  if (p >= jit->program_capacity) {
    if (!create) {
      return NULL;
    }
    size_t capacity = jit->program_capacity ? jit->program_capacity : 2u;
    while (capacity <= p) {
      capacity *= 2u;
    }
    GLTANG_JitProgram * grown = gcu_allocator_realloc(exec->allocator, jit->programs, capacity * sizeof(GLTANG_JitProgram));
    if (!grown) {
      return NULL;
    }
    memset(grown + jit->program_capacity, 0, (capacity - jit->program_capacity) * sizeof(GLTANG_JitProgram));
    jit->programs = grown;
    jit->program_capacity = capacity;
  }
  GLTANG_JitProgram * program = &jit->programs[p];
  if (!program->fns) {
    if (!create) {
      return NULL;
    }
    uint32_t count = exec->programs[p].program->function_count;
    program->fns = gcu_allocator_calloc(exec->allocator, count ? count : 1u, sizeof(GLTANG_JitFn));
    if (!program->fns) {
      return NULL;
    }
    program->count = count;
  }
  return f < program->count ? &program->fns[f] : NULL;
}

void gltang_jit_attach(GLTANG_Execution * exec) {
  if (exec->jit) {
    return;
  }
  GRCORE_Context * context = exec->context;
  GLTANG_Jit * jit = gcu_allocator_calloc(exec->allocator, 1, sizeof(GLTANG_Jit));
  if (!jit) {
    return;
  }
  GRCORE_Result r = grcore_context_request_kind(context, &tierup_key, &jit->kind);
  if (r == GRCORE_OK) {
    // Made once, empty: each compiled call extends it by its callee's converting
    // locations, which lang-tang's code does not have (every frame state holds
    // plain words and values), so the extension is always zero.
    r = grcore_deopt_reserve(context, 0, &jit->reservation);
  }
  if (r == GRCORE_OK) {
    r = grcore_context_register(context, &tierup_key, exec);
    if (r != GRCORE_OK) {
      grcore_deopt_release(context, jit->reservation);
    }
  }
  if (r != GRCORE_OK) {
    gcu_allocator_free(exec->allocator, jit);
    return;
  }
  exec->jit = jit;
  exec->jit_threshold = GLTANG_JIT_DEFAULT_THRESHOLD;
  exec->jit_settled_fword = UINT64_MAX;
}

/**
 * Lets a function's code go: its slot is refused (so callers' calls become
 * remembered exits), its range unregistered, and the cache's reference dropped.
 * Nothing is destroyed here: the registry and the slot retire their references
 * and core frees the code when no JIT record is open.
 */
static void discard(GLTANG_Execution * exec, GLTANG_JitFn * f) {
  if (f->slot) {
    (void)grcore_entry_slot_refuse(exec->context, f->slot);
  }
  if (f->start) {
    (void)grcore_code_unregister(exec->context, f->start);
    f->start = 0;
  }
  grcore_code_release(f->code);
  f->code = NULL;
  f->state = GLTANG_JITFN_NEVER;
}

void gltang_jit_release(GLTANG_Execution * exec) {
  GLTANG_Jit * jit = exec->jit;
  if (!jit) {
    return;
  }
  exec->jit = NULL;
  exec->jit_threshold = 0;
  for (size_t p = 0; p < jit->program_capacity; ++p) {
    GLTANG_JitFn * fns = jit->programs[p].fns;
    for (uint32_t f = 0; fns && f < jit->programs[p].count; ++f) {
      if (fns[f].slot) {
        // The slot stays with the context; the code it held is retired.
        (void)grcore_entry_slot_clear(exec->context, fns[f].slot);
      }
      if (fns[f].start) {
        (void)grcore_code_unregister(exec->context, fns[f].start);
        fns[f].start = 0;
      }
      if (fns[f].code) {
        grcore_code_release(fns[f].code);
        fns[f].code = NULL;
      }
    }
    gcu_allocator_free(exec->allocator, fns);
  }
  (void)grcore_deopt_release(exec->context, jit->reservation);
  jit->reservation = NULL;
  gcu_allocator_free(exec->allocator, jit->programs);
  gcu_allocator_free(exec->allocator, jit->queue);
  grcore_port_release(jit->port);
  gcu_allocator_free(exec->allocator, jit);
}

GRCORE_EntrySlot * gltang_jit_slot_for(GLTANG_Execution * exec, uint64_t fword) {
  GLTANG_Jit * jit = exec->jit;
  GLTANG_JitFn * f = jit ? gltang_jit_find_fn(exec, jit, fword, true) : NULL;
  if (!f) {
    return NULL;
  }
  if (!f->slot) {
    if (grcore_entry_slot_create(exec->context, &f->slot) != GRCORE_OK) {
      f->slot = NULL;
      return NULL;
    }
    if (f->state == GLTANG_JITFN_NEVER) {
      (void)grcore_entry_slot_refuse(exec->context, f->slot);
    }
  }
  return f->slot;
}

void gltang_jit_mark_never(GLTANG_Execution * exec, GLTANG_Jit * jit, GLTANG_JitFn * f) {
  (void)jit;
  f->state = GLTANG_JITFN_NEVER;
  if (f->slot) {
    (void)grcore_entry_slot_refuse(exec->context, f->slot);
  }
}

bool gltang_jit_compile_function(GLTANG_Execution * exec, uint64_t fword) {
  GLTANG_Jit * jit = exec->jit;
  GLTANG_JitFn * f = jit ? gltang_jit_find_fn(exec, jit, fword, true) : NULL;
  if (!f) {
    return false;
  }
  if (f->state == GLTANG_JITFN_COMPILED && f->code) {
    return true;
  }
  if (f->state == GLTANG_JITFN_NEVER) {
    if (f->slot) {
      (void)grcore_entry_slot_refuse(exec->context, f->slot);
    }
    return false;
  }
  GRCORE_Code * handle = NULL;
  GLTANG_Result r = gltang_jit_build(exec, GLTANG_FN_PROGRAM(fword), GLTANG_FN_INDEX(fword), &handle);
  if (r != GLTANG_OK) {
    ++exec->jit_stats.compile_failures;
    gltang_jit_mark_never(exec, jit, f);
    return false;
  }
  if (!handle) {
    gltang_jit_mark_never(exec, jit, f);
    return false;
  }
  const GLTANG_JitCode * jc = grcore_code_payload(handle);
  uintptr_t start = (uintptr_t)grjit_code_address(jc->code);
  if (grcore_code_register(exec->context, exec->engine, handle, start, grjit_code_size(jc->code), grjit_code_meta(jc->code)) != GRCORE_OK) {
    grcore_code_release(handle);
    ++exec->jit_stats.compile_failures;
    gltang_jit_mark_never(exec, jit, f);
    return false;
  }
  GRCORE_EntrySlot * slot = gltang_jit_slot_for(exec, fword);
  if (!slot || grjit_entry_slot_install(exec->context, slot, handle, jc->code, fword, (size_t)jc->parameter_count + 1u) != GRJIT_OK) {
    (void)grcore_code_unregister(exec->context, start);
    grcore_code_release(handle);
    ++exec->jit_stats.compile_failures;
    gltang_jit_mark_never(exec, jit, f);
    return false;
  }
  f->code = handle;
  f->start = start;
  f->state = GLTANG_JITFN_COMPILED;
  ++exec->jit_stats.functions_compiled;
  return true;
}

static void never(GLTANG_Execution * exec, GLTANG_JitFn * f) {
  gltang_jit_mark_never(exec, exec->jit, f);
}

void gltang_jit_note_poll(GLTANG_Execution * exec, uint64_t fword, bool at_entry) {
  if (fword == GLTANG_FN_WORD(0, 0) && !at_entry) {
    return;
  }
  GLTANG_Jit * jit = exec->jit;
  GLTANG_JitFn * f = jit->last_fn;
  if (!f || jit->last_fword != fword) {
    f = gltang_jit_find_fn(exec, jit, fword, true);
    if (!f) {
      // The bookkeeping could not be allocated: the interpreter runs alone from
      // here, which is always correct.
      exec->jit_threshold = 0;
      return;
    }
    jit->last_fword = fword;
    jit->last_fn = f;
  }
  if (f->state == GLTANG_JITFN_NEVER) {
    // Settled: the interpreter stops asking about this function.
    exec->jit_settled_fword = fword;
    return;
  }
  if (f->state != GLTANG_JITFN_COLD || ++f->polls < exec->jit_threshold) {
    return;
  }
  if (jit->queue_count == jit->queue_capacity) {
    size_t capacity = jit->queue_capacity ? jit->queue_capacity * 2u : 8u;
    uint64_t * grown = gcu_allocator_realloc(exec->allocator, jit->queue, capacity * sizeof(uint64_t));
    if (!grown) {
      never(exec, f);
      return;
    }
    jit->queue = grown;
    jit->queue_capacity = capacity;
  }
  if (!jit->port && grcore_context_port(exec->context, &jit->port) != GRCORE_OK) {
    jit->port = NULL;
    never(exec, f);
    return;
  }
  if (grcore_port_post(jit->port, jit->kind) != GRCORE_OK) {
    never(exec, f);
    return;
  }
  f->state = GLTANG_JITFN_QUEUED;
  jit->queue[jit->queue_count++] = fword;
}

static void tierup_poll(GRCORE_Context * context, void * value, GRCORE_PollCall * call) {
  GLTANG_Execution * exec = value;
  if (!exec || exec->destroyed || !exec->jit) {
    return;
  }
  GLTANG_Jit * jit = exec->jit;
  if (!grcore_pollcall_pending(call, jit->kind)) {
    return;
  }
  // ACT carries a verdict out. This handler only ever acts on continue: a
  // pause or an unwind leaves the request pending and the queue as it is, and
  // the next poll that continues compiles (AD-5).
  if (grcore_pollcall_verdict(call) != GRCORE_VERDICT_CONTINUE) {
    return;
  }
  for (size_t i = 0; i < jit->queue_count; ++i) {
    uint64_t fword = jit->queue[i];
    GLTANG_JitFn * f = gltang_jit_find_fn(exec, jit, fword, false);
    if (!f || f->state != GLTANG_JITFN_QUEUED) {
      continue;
    }
    (void)gltang_jit_compile_function(exec, fword);
  }
  jit->queue_count = 0;
  (void)grcore_context_clear_request(context, jit->kind);
}

void gltang_jit_unwound(GLTANG_Execution * exec) {
  GLTANG_Jit * jit = exec->jit;
  if (!jit) {
    return;
  }
  jit->queue_count = 0;
  (void)grcore_context_clear_request(exec->context, jit->kind);
}

// ---------------------------------------------------------------------------
// Entering compiled code
// ---------------------------------------------------------------------------

GLTANG_JitExit gltang_jit_enter(GLTANG_Execution * exec, GRCORE_Context * context, uint64_t fword, GLTANG_Value * out_value) {
  GLTANG_Jit * jit = exec->jit;
  if (!jit) {
    return GLTANG_JIT_NOT_ENTERED;
  }
  // The poll that has just run left this function in the one-entry cache, so
  // the common answer (a function that is not compiled) costs no lookup.
  GLTANG_JitFn * f = jit->last_fn != NULL && jit->last_fword == fword ? jit->last_fn : gltang_jit_find_fn(exec, jit, fword, false);
  if (!f || f->state != GLTANG_JITFN_COMPILED) {
    return GLTANG_JIT_NOT_ENTERED;
  }
  GRCORE_Stack * stack = grcore_context_stack(context);
  // The native-depth budget (AD-21): a refusal means "not this time", and the
  // interpreter runs the function. Nothing else about the budget is touched.
  GRCORE_ActivationRef record;
  if (grcore_activation_enter(stack, GRCORE_ACTIVATION_JIT, exec->engine, false, NULL, &record) != GRCORE_OK) {
    return GLTANG_JIT_NOT_ENTERED;
  }
  // One reference for the duration of the call, so code discarded during the
  // run (a deopt that was the eighth) is freed only after the call ends.
  GRCORE_Code * handle = grcore_code_retain(f->code);
  const GLTANG_JitCode * jc = grcore_code_payload(handle);
  uint64_t args[GLTANG_JIT_MAX_SLOTS];
  uint64_t out[GLTANG_JIT_MAX_SLOTS + 2u];
  {
    // The parameters, then the hidden flag: 1 from here, because the interpreter
    // has made this function's entry poll; a compiled call passes 0 and the
    // callee makes its own.
    const uint64_t * S = grcore_stack_slots(stack, grcore_stack_top(stack));
    for (uint32_t k = 0; k < jc->parameter_count; ++k) {
      args[k] = S[H + k];
    }
    args[jc->parameter_count] = 1u;
  }
  ++exec->jit_stats.entries;
  jit->chain = 0;
  jit->exit_valid = false;
  jit->refusal = GLTANG_REFUSAL_NONE;
  uint32_t exit = grjit_code_call(jc->code, context, args, out);
  jit->chain = 0;
  // Left before the interpreter acts on the exit, with the rebuilt guest frames
  // in place (core allows that for a rebuilt JIT record: never fewer frames than
  // it began with).
  (void)grcore_activation_leave(stack, record);

  GLTANG_JitExit result = GLTANG_JIT_NOT_ENTERED;
  bool counted = false;
  switch (exit) {
    case GRJIT_EXIT_RETURNED:
      ++exec->jit_stats.returns;
      *out_value = out[0];
      result = GLTANG_JIT_RETURNED;
      break;
    case GRJIT_EXIT_DEOPT: {
      // Every compiled frame of the chain has been rebuilt into its guest frame
      // by the deopt hook; the cause is the poll helper's answer, or 0 for a
      // guard or an exit at a call.
      uint64_t cause = out[0];
      exec->jit_stats.last_exit_cause = cause;
      if (cause == 0) {
        ++exec->jit_stats.deopts;
        result = GLTANG_JIT_DEOPTED;
      }
      else if (cause == 1u) {
        ++exec->jit_stats.refused_pauses;
        result = GLTANG_JIT_PAUSED;
      }
      else {
        ++exec->jit_stats.refused_unwinds;
        result = GLTANG_JIT_UNWOUND;
      }
      counted = jit->exit_valid && jit->exit_counted;
      break;
    }
    case GRJIT_EXIT_REBUILD_FAILED:
      // Nothing was rebuilt: the guest frames are as the compiled calls left them,
      // which is not a state anything can finish. The run ends as an unwind of its
      // own (every frame it pushed is popped, nothing is interpreted on them, no
      // value is made), never continued.
      ++exec->jit_stats.rebuild_failures;
      exec->jit_stats.last_exit_cause = out[0];
      result = GLTANG_JIT_UNWOUND;
      break;
    default:
      // GRJIT_EXIT_REFUSED or a code this version does not know: neither can
      // occur for callable code (the entry has no hook of its own), so the same
      // fatal end.
      ++exec->jit_stats.rebuild_failures;
      exec->jit_stats.last_exit_cause = out[0];
      result = GLTANG_JIT_UNWOUND;
      break;
  }
  jit->exit_valid = false;
  grcore_code_release(handle);
  if (counted) {
    // The function whose frame was innermost at the exit is charged for it. At the
    // eighth it is let go: its slot is refused, its range unregistered and the
    // cache's reference dropped, and core frees the code when no record is open.
    GLTANG_JitFn * g = gltang_jit_find_fn(exec, jit, jit->exit_fword, false);
    if (g && g->state == GLTANG_JITFN_COMPILED && ++g->deopts >= GLTANG_JIT_DEOPT_LIMIT) {
      discard(exec, g);
      ++exec->jit_stats.functions_discarded;
    }
  }
  return result;
}

/*
 * For the tests only: three switches over the compiled calls of an execution
 * before it runs. `calls_off` compiles no call site (every `CALL` stays an exit,
 * as in milestone 1, which is the control of a measurement); `fail_rebuild`
 * makes the deopt hook's rebuild fail, which the library says an engine must
 * survive without continuing on unrebuilt frames; `gc_at_push` makes the push
 * hook collect after its push, which is the frame-push GC point GC torture
 * needs (the heap owns the GC points it has, not this one). Not declared in any
 * header; a test declares it itself.
 */
void gltang_vm_set_jit_test_switches_unchecked(GLTANG_Execution * execution, bool calls_off, bool fail_rebuild, bool gc_at_push);
void gltang_vm_set_jit_test_switches_unchecked(GLTANG_Execution * execution, bool calls_off, bool fail_rebuild, bool gc_at_push) {
  if (execution && execution->jit) {
    execution->jit->test_calls_off = calls_off;
    execution->jit->test_fail_rebuild = fail_rebuild;
    execution->jit->gc_at_push = gc_at_push;
  }
}

/*
 * For the tests only: offers the compiled code of one function of the main
 * program to the entry slot of another, with the token and the parameter count
 * `mode` picks, and says whether the install was refused and what the slot held
 * before and after. 0 uses the slot's token and count (so the token is another
 * function's), 1 the code's own token with one parameter too many, 2 the slot's
 * token and count with the other function's code, and 3 is the control: the
 * code's own token and count into its own slot. Returns 1 for a refusal, 0 for an
 * install, -1 when there is nothing to offer.
 */
int gltang_vm_jit_test_forged_install(GLTANG_Execution * execution, uint32_t code_fn, uint32_t slot_fn, int mode, uintptr_t * before, uintptr_t * after);
int gltang_vm_jit_test_forged_install(GLTANG_Execution * execution, uint32_t code_fn, uint32_t slot_fn, int mode, uintptr_t * before, uintptr_t * after) {
  GLTANG_Jit * jit = execution ? execution->jit : NULL;
  GLTANG_JitFn * code_record = jit ? gltang_jit_find_fn(execution, jit, GLTANG_FN_WORD(0, code_fn), false) : NULL;
  GLTANG_JitFn * slot_record = jit ? gltang_jit_find_fn(execution, jit, GLTANG_FN_WORD(0, slot_fn), false) : NULL;
  if (!code_record || !code_record->code || !slot_record || !slot_record->slot) {
    return -1;
  }
  const GLTANG_JitCode * jc = grcore_code_payload(code_record->code);
  const GLTANG_Function * slot_function = &execution->programs[0].program->functions[slot_fn];
  uint64_t token = GLTANG_FN_WORD(0, mode == 1 || mode == 3 ? code_fn : slot_fn);
  size_t count = mode == 1 ? (size_t)jc->parameter_count + 2u : mode == 3 ? (size_t)jc->parameter_count + 1u : (size_t)slot_function->parameter_count + 1u;
  *before = slot_record->slot->entry;
  GRJIT_Result r = grjit_entry_slot_install(execution->context, slot_record->slot, code_record->code, jc->code, token, count);
  *after = slot_record->slot->entry;
  return r == GRJIT_OK ? 0 : 1;
}

/*
 * For the tests only: the sites, the derived pointers and the converting
 * locations of every compiled function's metadata, which lang-tang's frames
 * must not have (a frame holds plain words and values, so the reservation each
 * call extends is always zero). Returns 1 if anything is compiled.
 */
int gltang_vm_jit_test_metadata(GLTANG_Execution * execution, uint64_t * sites, uint64_t * derived, uint64_t * converting);
int gltang_vm_jit_test_metadata(GLTANG_Execution * execution, uint64_t * sites, uint64_t * derived, uint64_t * converting) {
  GLTANG_Jit * jit = execution ? execution->jit : NULL;
  int any = 0;
  *sites = *derived = *converting = 0;
  for (size_t p = 0; jit && p < jit->program_capacity; ++p) {
    for (uint32_t f = 0; jit->programs[p].fns && f < jit->programs[p].count; ++f) {
      GLTANG_JitFn * fn = &jit->programs[p].fns[f];
      if (!fn->code) {
        continue;
      }
      const GRCORE_CodeMeta * meta = grjit_code_meta(((const GLTANG_JitCode *)grcore_code_payload(fn->code))->code);
      any = 1;
      for (size_t s = 0; s < meta->site_count; ++s) {
        ++*sites;
        *derived += meta->sites[s].derived_count;
        *converting += grcore_deopt_converting_count(&meta->sites[s]);
      }
    }
  }
  return any;
}

/*
 * For the tests only: calls one of the call hooks (0 push, 1 pop, 2 compile, 3
 * deopt) as compiled code would, with arguments the test chooses, so that the
 * checks a hook makes of its own arguments are seen to refuse. Returns the hook's
 * result.
 */
int gltang_vm_jit_test_hook(GLTANG_Execution * execution, int hook, uint64_t token, const uint64_t * args, uint64_t count);
int gltang_vm_jit_test_hook(GLTANG_Execution * execution, int hook, uint64_t token, const uint64_t * args, uint64_t count) {
  if (!execution || !execution->jit) {
    return -1;
  }
  switch (hook) {
    case 0: return (int)gltang_jit_hooks.push(execution->context, token, args, count);
    case 2: return (int)gltang_jit_hooks.compile(execution->context, token);
    default: return -1;
  }
}
