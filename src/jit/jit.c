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

static const GRCORE_Key tierup_key = {
  .name = "lang-tang tier-up",
  .cardinality = GRCORE_CARDINALITY_ONE,
  .phase = GRCORE_PHASE_ACT,
  .destroy = NULL,
  .poll = tierup_poll,
};

static GLTANG_JitFn * find_fn(GLTANG_Execution * exec, GLTANG_Jit * jit, uint64_t fword, bool create) {
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
    r = grcore_context_register(context, &tierup_key, exec);
  }
  if (r != GRCORE_OK) {
    gcu_allocator_free(exec->allocator, jit);
    return;
  }
  exec->jit = jit;
  exec->jit_threshold = GLTANG_JIT_DEFAULT_THRESHOLD;
  exec->jit_settled_fword = UINT64_MAX;
}

static void discard(GLTANG_JitFn * f) {
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
      if (fns[f].code) {
        discard(&fns[f]);
      }
    }
    gcu_allocator_free(exec->allocator, fns);
  }
  gcu_allocator_free(exec->allocator, jit->programs);
  gcu_allocator_free(exec->allocator, jit->queue);
  grcore_port_release(jit->port);
  gcu_allocator_free(exec->allocator, jit);
}

static void never(GLTANG_Execution * exec, GLTANG_JitFn * f) {
  (void)exec;
  f->state = GLTANG_JITFN_NEVER;
}

void gltang_jit_note_poll(GLTANG_Execution * exec, uint64_t fword, bool at_entry) {
  if (fword == GLTANG_FN_WORD(0, 0) && !at_entry) {
    return;
  }
  GLTANG_Jit * jit = exec->jit;
  GLTANG_JitFn * f = jit->last_fn;
  if (!f || jit->last_fword != fword) {
    f = find_fn(exec, jit, fword, true);
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
    GLTANG_JitFn * f = find_fn(exec, jit, fword, false);
    if (!f || f->state != GLTANG_JITFN_QUEUED) {
      continue;
    }
    GRCORE_Code * code = NULL;
    GLTANG_Result r = gltang_jit_build(exec, GLTANG_FN_PROGRAM(fword), GLTANG_FN_INDEX(fword), &code);
    if (r != GLTANG_OK) {
      ++exec->jit_stats.compile_failures;
      never(exec, f);
    }
    else if (!code) {
      never(exec, f);
    }
    else {
      f->code = code;
      f->state = GLTANG_JITFN_COMPILED;
      ++exec->jit_stats.functions_compiled;
    }
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
  GLTANG_JitFn * f = jit->last_fn != NULL && jit->last_fword == fword ? jit->last_fn : find_fn(exec, jit, fword, false);
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
    const uint64_t * S = grcore_stack_slots(stack, grcore_stack_top(stack));
    for (uint32_t k = 0; k < jc->local_count; ++k) {
      args[k] = S[H + k];
    }
  }
  ++exec->jit_stats.entries;
  const GRJIT_Code * outer = jit->running;
  jit->running = jc->code;
  uint32_t exit = grjit_code_call(jc->code, context, args, out);
  jit->running = outer;
  // Left before the interpreter acts on the exit. The guest frame count is
  // what it was when the record was entered, because compiled code never
  // pushes or pops a frame, so the leave is legal.
  (void)grcore_activation_leave(stack, record);

  GLTANG_JitExit result = GLTANG_JIT_NOT_ENTERED;
  bool thrash = false;
  switch (exit) {
    case GRJIT_EXIT_RETURNED:
      ++exec->jit_stats.returns;
      *out_value = out[0];
      result = GLTANG_JIT_RETURNED;
      break;
    case GRJIT_EXIT_DEOPT: {
      // The frame state, in the guest frame's own slot order: function (constant),
      // pc, sp, flags (dead: the frame keeps its own), the locals, the operand
      // stack, and last the fuel compiled code had counted and not yet charged.
      uint64_t * S = grcore_stack_slots(stack, grcore_stack_top(stack));
      S[GLTANG_F_PC] = out[GLTANG_F_PC];
      S[GLTANG_F_SP] = out[GLTANG_F_SP];
      for (uint32_t k = H; k < jc->frame_slots; ++k) {
        S[k] = out[k];
      }
      exec->pending_fuel += out[jc->frame_slots];
      ++exec->jit_stats.deopts;
      thrash = ++f->deopts >= GLTANG_JIT_DEOPT_LIMIT;
      result = GLTANG_JIT_DEOPTED;
      break;
    }
    default:
      // GRJIT_EXIT_REFUSED: the poll helper's answer, with the guest frame
      // already current.
      if (out[0] == 1u) {
        ++exec->jit_stats.refused_pauses;
        result = GLTANG_JIT_PAUSED;
      }
      else {
        ++exec->jit_stats.refused_unwinds;
        result = GLTANG_JIT_UNWOUND;
      }
      break;
  }
  grcore_code_release(handle);
  if (thrash && f->state == GLTANG_JITFN_COMPILED) {
    // Deoptimized too often to be worth having: the code is let go (freed
    // here, now that the call is over) and the function is never compiled
    // again in this execution.
    discard(f);
    ++exec->jit_stats.functions_discarded;
  }
  return result;
}
