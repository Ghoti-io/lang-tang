/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
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
 * Pending records: what a suspended host callback leaves (AD-23, type.h).
 *
 * A callback that asks for a guest call (::gltang_call_guest) returns, and its
 * bracket closes: no scope, handle, pin, text pointer, native-depth unit or
 * activation record of it stays. What stays is one heap object, the *pending
 * record*, held by the execution (`pending_top`, a root) and chained to the one
 * below it. It holds the continuation's index, the state the callback named, the
 * dispatch step, the owner, the requested call, and a slot for the callee's
 * value. The collector, a pause, a resume on any thread and an unwind find it
 * with no C state, because there is none.
 *
 * The callee's frame is the one `deliver_frame` names: when it returns, the
 * interpreter hands the value to the innermost record (gltang_vm_pending_deliver)
 * and runs the instruction again; the operation replays its earlier dispatch
 * steps as declines and calls the continuation at the step the record holds
 * (custom.c, libvalue.c). That is the whole of the protocol; this file is the
 * record's life.
 */

#include <ghoti.io/lang-tang/macros.h>

#include "test_hooks.h"
#include "vm_internal.h"

static void pending_trace(GRHEAP_Tracer * tracer, void * payload) {
  GLTANG_PendingObject * object = payload;
  for (uint32_t i = 0; i < object->slot_count; ++i) {
    grheap_trace_word(tracer, &object->slots[i]);
  }
}

const GRHEAP_Type gltang_type_pending = GRHEAP_TYPE_INIT("lang-tang pending", sizeof(GLTANG_PendingObject), pending_trace, NULL, NULL, gltang_vm_pending_snapshot, NULL);

GRHEAP_Result gltang_vm_pending_snapshot(GRCORE_Context * context, void * payload, GRHEAP_ImageWriter * writer) {
  (void)writer;
  // The record names its owner by address. Writing it by name and version needs the
  // by-name mechanism of host types (story 8), which does not exist yet: the image is
  // refused, and names the type when there is one.
  const GLTANG_PendingObject * record = payload;
  GLTANG_Execution * exec = gltang_vm_execution_of(context);
  if (exec && exec->handles && !exec->handles->snapshot_refused) {
    exec->handles->snapshot_refused = record->type;
  }
  return GRHEAP_ERR_INVALID;
}

static inline GLTANG_PendingObject * record_of(GLTANG_Value v) {
  return (GLTANG_PendingObject *)gltang_object(v);
}

size_t gltang_vm_pending_depth(const GLTANG_Execution * exec) {
  return exec->pending_top ? record_of(exec->pending_top)->depth : 0;
}

GLTANG_Value gltang_vm_pending_push(GLTANG_Execution * exec, const GLTANG_PendingSpec * spec) {
  const size_t slots = GLTANG_PENDING_ARGS + spec->argc + spec->operand_count;
  void * object;
  GLTANG_Status st = gltang_vm_alloc(exec, &gltang_type_pending, sizeof(GLTANG_PendingObject) + slots * sizeof(GLTANG_Value), &object);
  if (st != GLTANG_ST_OK) {
    return gltang_vm_failure_value(exec, st);
  }
  GLTANG_PendingObject * record = object;
  record->kind = GLTANG_OBJ_PENDING;
  record->slot_count = (uint16_t)slots;
  record->flags = spec->chained ? GLTANG_PENDING_FLAG_CHAINED : 0u;
  record->op = (uint8_t)spec->op;
  record->step = (uint16_t)spec->step;
  record->request_argc = (uint16_t)spec->argc;
  record->continuation = spec->continuation;
  record->operand_count = (uint32_t)spec->operand_count;
  record->type = spec->type;
  record->member = spec->member;
  record->callee_frame = 0;
  // The previous record is read after the allocation: the execution holds it, and a collection
  // rewrote that.
  record->depth = exec->pending_top ? record_of(exec->pending_top)->depth + 1u : 1u;
  GRHEAP_Heap * heap = exec->heap;
  // The values are read after the allocation: from the temporaries, which a collection updated, or from memory it
  // does not move.
  const GLTANG_Value state = spec->in_temps ? gltang_vm_temp_at(exec, spec->temp_base) : spec->state;
  const GLTANG_Value callee = spec->in_temps ? gltang_vm_temp_at(exec, spec->temp_base + 1u) : spec->callee;
  (void)grheap_store_word(heap, record, &record->slots[GLTANG_PENDING_STATE], state);
  (void)grheap_store_word(heap, record, &record->slots[GLTANG_PENDING_PREVIOUS], exec->pending_top);
  (void)grheap_store_word(heap, record, &record->slots[GLTANG_PENDING_CALLEE], callee);
  for (size_t i = 0; i < spec->argc; ++i) {
    const GLTANG_Value arg = spec->in_temps ? gltang_vm_temp_at(exec, spec->temp_base + 2u + i) : spec->args[i];
    (void)grheap_store_word(heap, record, &record->slots[GLTANG_PENDING_ARGS + i], arg);
  }
  for (size_t i = 0; i < spec->operand_count; ++i) {
    (void)grheap_store_word(heap, record, &record->slots[GLTANG_PENDING_ARGS + spec->argc + i], spec->operands[i]);
  }
  exec->pending_top = gltang_value_of(record);
  // A new record ends any delivery: the record delivered was let go before its continuation could ask again.
  exec->resume_active = false;
  return exec->pending_top;
}

void gltang_vm_pending_pop(GLTANG_Execution * exec) {
  if (!exec->pending_top) {
    return;
  }
  const GLTANG_PendingObject * record = record_of(exec->pending_top);
  GLTANG_Value previous = record->slots[GLTANG_PENDING_PREVIOUS];
  exec->pending_top = previous;
  exec->deliver_frame = previous ? record_of(previous)->callee_frame : 0;
}

void gltang_vm_pending_unconsumed(GLTANG_Execution * exec) {
  // The value was delivered (resume_active) and no continuation took it: nothing is left to continue.
  gltang_vm_pending_pop(exec);
  exec->resume_active = false;
}

void gltang_vm_pending_drop_to(GLTANG_Execution * exec, size_t depth) {
  while (exec->pending_top && record_of(exec->pending_top)->depth > depth) {
    gltang_vm_pending_pop(exec);
  }
  exec->resume_active = false;
  exec->request_made = false;
}

void gltang_vm_pending_set_callee_frame(GLTANG_Execution * exec, size_t offset) {
  record_of(exec->pending_top)->callee_frame = offset;
  exec->deliver_frame = offset;
}

void gltang_vm_pending_deliver(GLTANG_Execution * exec, GLTANG_Value result) {
  GLTANG_PendingObject * record = record_of(exec->pending_top);
  (void)grheap_store_word(exec->heap, record, &record->slots[GLTANG_PENDING_RESULT], result);
  exec->resume_active = true;
}

/*
 * For the tests only: how many records are open, and how many requests were run nested and suspended.
 * Declared in vm/test_hooks.h.
 */
uint64_t gltang_vm_test_pending_depth(const GLTANG_Execution * execution) {
  return execution ? gltang_vm_pending_depth(execution) : 0;
}

void gltang_vm_test_request_counts(const GLTANG_Execution * execution, uint64_t * nested, uint64_t * suspended) {
  const GLTANG_HandleState * hs = execution ? execution->handles : NULL;
  if (nested) {
    *nested = hs ? hs->nested_requests : 0;
  }
  if (suspended) {
    *suspended = hs ? hs->suspensions : 0;
  }
}
