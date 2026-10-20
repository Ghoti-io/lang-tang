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
 * Custom values (type.h): the registries an execution attaches, making an
 * instance, and the dispatch every operation of the language falls through to
 * for a custom operand, with ctang's reflection rule (planning section 5.3,
 * spine AD-29).
 *
 * **One bracket for every operation.** A type's operation is a host callback
 * (AD-23), entered the way `gltang_vm_call_native` enters a native: its
 * operands are copied into this C frame, which the native activation record
 * gives the collector as a conservative segment (so what they name is pinned
 * for the call), the record takes a unit of native depth, the call object
 * opens a handle scope with a handle for each operand, `in_host` is set, and
 * the answer is read while that scope is open. A refused record is the call's
 * value, as for a native.
 *
 * **A decline is a status.** It is reported to the caller as a flag beside a
 * null value, never as an error object: nothing is allocated for it and the
 * error-created hook never sees it, so a decline cannot halt the run or appear
 * in the error list. The language's `Not supported` is made once, here, after
 * every side that could answer has declined.
 */

#include <ghoti.io/lang-tang/macros.h>

#include <stdatomic.h>
#include <string.h>
#include "vm_internal.h"

#define MAKE_ERROR(kind) gltang_vm_make_error(exec, (kind))

/** The most operands one operation is given: `self`, and up to three more (a slice's parts). */
#define OPERANDS_MAX 4u

// ---------------------------------------------------------------------------
// Registries
// ---------------------------------------------------------------------------

/**
 * The serial of the next context to attach a registry. An execution is its
 * context's only one (a context holds one execution key), so the serial is
 * given to the execution and names the context for the binding: an address
 * would not, since a later context can be given a freed one.
 */
static atomic_uint_fast64_t next_context_serial = 1;

bool gltang_vm_type_attached(const GLTANG_Execution * exec, const GLTANG_Type * type) {
  const GLTANG_HandleState * hs = exec->handles;
  if (!hs || !type) {
    return false;
  }
  for (size_t i = 0; i < hs->registry_count; ++i) {
    if (hs->registries[i] == type->registry) {
      return true;
    }
  }
  return false;
}

/** Whether `incoming` holds a name and version an attached registry holds with another table hash. */
static bool clashes(const GLTANG_HandleState * hs, const GLTANG_TypeRegistry * incoming) {
  for (size_t r = 0; r < hs->registry_count; ++r) {
    const GLTANG_TypeRegistry * held = hs->registries[r];
    for (size_t i = 0; i < incoming->count; ++i) {
      const GLTANG_Type * t = incoming->types[i];
      const GLTANG_Type * other = gltang_type_registry_find(held, t->name, t->descriptor.version);
      if (other && other->table_hash != t->table_hash) {
        return true;
      }
    }
  }
  return false;
}

GLTANG_Result gltang_execution_attach_types(GLTANG_Execution * execution, GLTANG_TypeRegistry * registry) {
  if (!registry || !execution || execution->destroyed || execution->in_host || execution->state != GLTANG_EXECUTION_NEW) {
    return GLTANG_ERR_INVALID;
  }
  GLTANG_HandleState * hs = gltang_vm_handle_state(execution);
  if (!hs) {
    return GLTANG_ERR_OOM;
  }
  for (size_t i = 0; i < hs->registry_count; ++i) {
    if (hs->registries[i] == registry) {
      return GLTANG_OK;
    }
  }
  if (clashes(hs, registry)) {
    return GLTANG_ERR_INVALID;
  }
  if (hs->registry_count == hs->registry_capacity) {
    size_t grown = hs->registry_capacity ? hs->registry_capacity * 2u : 4u;
    GLTANG_TypeRegistry ** bigger = gcu_allocator_realloc(execution->allocator, hs->registries, grown * sizeof(*bigger));
    if (!bigger) {
      return GLTANG_ERR_OOM;
    }
    hs->registries = bigger;
    hs->registry_capacity = grown;
  }
  // A registry of thread-unsafe types belongs to the first context that takes
  // it; the binding is checked before anything is changed, so a refusal leaves
  // the registry as it was.
  if (!hs->context_serial) {
    hs->context_serial = (uint64_t)atomic_fetch_add_explicit(&next_context_serial, 1, memory_order_relaxed);
  }
  if (!gltang_type_registry_bind(registry, hs->context_serial)) {
    return GLTANG_ERR_INVALID;
  }
  hs->registries[hs->registry_count++] = gltang_type_registry_attach(registry);
  return GLTANG_OK;
}

// ---------------------------------------------------------------------------
// Instances
// ---------------------------------------------------------------------------

GLTANG_Value gltang_vm_make_custom(GLTANG_Execution * exec, const GLTANG_Type * type) {
  uint32_t slots = type->descriptor.slot_count;
  size_t bytes = sizeof(GLTANG_CustomObject) + (size_t)slots * sizeof(GLTANG_Value) + (size_t)type->descriptor.byte_count;
  void * object;
  GLTANG_Status st = gltang_vm_alloc(exec, &gltang_type_custom, bytes, &object);
  if (st != GLTANG_ST_OK) {
    return gltang_vm_failure_value(exec, st);
  }
  // The heap zero-fills a payload: every slot is null and every byte zero.
  GLTANG_CustomObject * custom = object;
  custom->kind = GLTANG_OBJ_CUSTOM;
  custom->slot_count = slots;
  custom->type = type;
  return gltang_value_of(custom);
}

GRHEAP_Result gltang_vm_custom_snapshot(GRCORE_Context * context, void * payload, GRHEAP_ImageWriter * writer) {
  (void)writer;
  // The payload is a copy whose type pointer is intact. The type has no way yet
  // to be written by name and restored (story 8), so the image is refused, and
  // the refusal names the type: the one diagnostic this hook leaves.
  const GLTANG_CustomObject * custom = payload;
  GLTANG_Execution * exec = gltang_vm_execution_of(context);
  if (exec && exec->handles && !exec->handles->snapshot_refused) {
    exec->handles->snapshot_refused = custom->type;
  }
  return GRHEAP_ERR_INVALID;
}

// ---------------------------------------------------------------------------
// The bracket
// ---------------------------------------------------------------------------

/** An operation's operands and flags, as they are handed to the callback. */
typedef struct OpCall {
  const GLTANG_Type * type;
  GLTANG_TypeOp op;
  size_t argc;
  bool self_is_lhs;
  bool is_assignment;
} OpCall;

/** Calls the callback with the call object and the handles made for `args`. */
static GLTANG_OpStatus run_callback(GLTANG_NativeCall * call, const OpCall * oc, const GLTANG_Handle * h) {
  const GLTANG_TypeDescriptor * d = &oc->type->descriptor;
  void * user = d->user;
  switch (oc->op) {
    case GLTANG_TYPE_OP_ADD: return d->add(call, user, h[0], h[1], oc->self_is_lhs, oc->is_assignment);
    case GLTANG_TYPE_OP_SUBTRACT: return d->subtract(call, user, h[0], h[1], oc->self_is_lhs, oc->is_assignment);
    case GLTANG_TYPE_OP_MULTIPLY: return d->multiply(call, user, h[0], h[1], oc->self_is_lhs, oc->is_assignment);
    case GLTANG_TYPE_OP_DIVIDE: return d->divide(call, user, h[0], h[1], oc->self_is_lhs, oc->is_assignment);
    case GLTANG_TYPE_OP_MODULO: return d->modulo(call, user, h[0], h[1], oc->self_is_lhs, oc->is_assignment);
    case GLTANG_TYPE_OP_NEGATE: return d->negate(call, user, h[0]);
    case GLTANG_TYPE_OP_LESS: return d->less(call, user, h[0], h[1], oc->self_is_lhs);
    case GLTANG_TYPE_OP_LESS_EQUAL: return d->less_equal(call, user, h[0], h[1], oc->self_is_lhs);
    case GLTANG_TYPE_OP_GREATER: return d->greater(call, user, h[0], h[1], oc->self_is_lhs);
    case GLTANG_TYPE_OP_GREATER_EQUAL: return d->greater_equal(call, user, h[0], h[1], oc->self_is_lhs);
    case GLTANG_TYPE_OP_EQUAL: return d->equal(call, user, h[0], h[1], oc->self_is_lhs);
    case GLTANG_TYPE_OP_NOT_EQUAL: return d->not_equal(call, user, h[0], h[1], oc->self_is_lhs);
    case GLTANG_TYPE_OP_ATTRIBUTE_GET: return d->attribute_get(call, user, h[0], h[1]);
    case GLTANG_TYPE_OP_ATTRIBUTE_SET: return d->attribute_set(call, user, h[0], h[1], h[2]);
    case GLTANG_TYPE_OP_INDEX_GET: return d->index_get(call, user, h[0], h[1]);
    case GLTANG_TYPE_OP_INDEX_SET: return d->index_set(call, user, h[0], h[1], h[2]);
    case GLTANG_TYPE_OP_SLICE: return d->slice(call, user, h[0], h[1], h[2], h[3]);
    case GLTANG_TYPE_OP_COUNT: break;
  }
  return GLTANG_OP_ERROR;
}

/**
 * Calls one operation of a type. `args` are the operands, `self` first. The
 * answer is the value; `*declined` says the callback declined, and then the
 * value is null and nothing was made. A GC point.
 */
static GLTANG_Value invoke(GLTANG_Execution * exec, const OpCall * oc, const GLTANG_Value * args, bool * declined) {
  *declined = false;
  // The operands are copied into this frame first; the record gives exactly the
  // copies as its conservative segment, so the objects they name are pinned and
  // the callback reads the copies (as gltang_vm_call_native does).
  GLTANG_Value held[OPERANDS_MAX] = {0};
  memcpy(held, args, oc->argc * sizeof(GLTANG_Value));
  // Charged as a native call; the bytes of a string answer are charged below.
  exec->pending_fuel += gltang_opcode_cost_table[GLTANG_OP_CALL];
  GRCORE_Stack * stack = grcore_context_stack(exec->context);
  GRCORE_CSegment segment = {(uintptr_t)&held[0], (uintptr_t)&held[0] + sizeof held};
  GRCORE_ActivationRef record;
  GRCORE_Result entered = grcore_activation_enter(stack, GRCORE_ACTIVATION_NATIVE, exec->engine, false, &segment, &record);
  if (entered != GRCORE_OK) {
    return gltang_vm_native_refused_value(exec, entered);
  }
  if (exec->native_gc_seam) {
    // Entering a nested activation is a GC point (AD-17), made here under torture.
    (void)grheap_collect(exec->heap);
  }
  GLTANG_NativeCall call;
  memset(&call, 0, sizeof(call));
  call.exec = exec;
  call.args = held;
  call.argc = oc->argc;
  GLTANG_Handle h[OPERANDS_MAX];
  memset(h, 0, sizeof h);
  bool made = true;
  for (size_t i = 0; i < oc->argc && made; ++i) {
    made = gltang_call_arg(&call, i, &h[i]) == GLTANG_OK;
  }
  GLTANG_OpStatus status = GLTANG_OP_ERROR;
  if (made) {
    // Host code from here to its return: it cannot destroy or reconfigure the
    // execution (the setters refuse), as in a native.
    const bool outer_in_host = exec->in_host;
    exec->in_host = true;
    status = run_callback(&call, oc, h);
    exec->in_host = outer_in_host;
  }
  GLTANG_Value result;
  if (!call.scope_unwind && !call.scope_oom && status == GLTANG_OP_DECLINE) {
    // A decline: whatever the callback set is dropped, and no value is made.
    gltang_vm_call_abandon(&call);
    *declined = true;
    result = GLTANG_V_NULL;
  }
  else {
    if (status == GLTANG_OP_OK && call.set == GLTANG_CALL_NONE &&
        (oc->op == GLTANG_TYPE_OP_ATTRIBUTE_SET || oc->op == GLTANG_TYPE_OP_INDEX_SET)) {
      // An assignment with no answer of its own is worth the value assigned.
      call.set = GLTANG_CALL_VALUE;
      call.value = held[2];
    }
    if (status != GLTANG_OP_OK && call.set != GLTANG_CALL_ERROR) {
      // An error with no kind set, or a status this engine does not know.
      gltang_call_return_error(&call, GLTANG_ERROR_HOST_FAILED);
    }
    result = gltang_vm_call_settle(exec, &call, true);
    if (gltang_v_is_kind(result, GLTANG_OBJ_STRING)) {
      // A callback costs fuel in proportion to the bytes it answers, as a native does.
      exec->pending_fuel += gltang_vm_string(result)->byte_length / GLTANG_WORK_BYTES_PER_FUEL;
    }
  }
  (void)grcore_activation_leave(stack, record);
  return result;
}

// ---------------------------------------------------------------------------
// Binary operators and comparisons, with reflection
// ---------------------------------------------------------------------------

static bool op_of(GLTANG_Opcode op, GLTANG_TypeOp * out) {
  switch (op) {
    case GLTANG_OP_ADD: *out = GLTANG_TYPE_OP_ADD; return true;
    case GLTANG_OP_SUB: *out = GLTANG_TYPE_OP_SUBTRACT; return true;
    case GLTANG_OP_MUL: *out = GLTANG_TYPE_OP_MULTIPLY; return true;
    case GLTANG_OP_DIV: *out = GLTANG_TYPE_OP_DIVIDE; return true;
    case GLTANG_OP_MOD: *out = GLTANG_TYPE_OP_MODULO; return true;
    case GLTANG_OP_LT: *out = GLTANG_TYPE_OP_LESS; return true;
    case GLTANG_OP_LE: *out = GLTANG_TYPE_OP_LESS_EQUAL; return true;
    case GLTANG_OP_GT: *out = GLTANG_TYPE_OP_GREATER; return true;
    case GLTANG_OP_GE: *out = GLTANG_TYPE_OP_GREATER_EQUAL; return true;
    case GLTANG_OP_EQ: *out = GLTANG_TYPE_OP_EQUAL; return true;
    case GLTANG_OP_NE: *out = GLTANG_TYPE_OP_NOT_EQUAL; return true;
    default: return false;
  }
}

/**
 * The reflection rule for one operation over the pair held at temporaries
 * `mark` (the left operand) and `mark + 1` (the right): the left operand's
 * operation with `self_is_lhs` true, then, if it declined or the left is not
 * custom, the right operand's with the operands swapped and `self_is_lhs`
 * false. `*declined` is true when no side answered; nothing has been made then.
 */
static GLTANG_Value reflect(GLTANG_Execution * exec, size_t mark, GLTANG_TypeOp op, bool is_assignment, bool * declined) {
  *declined = true;
  GLTANG_Value a = gltang_vm_temp_at(exec, mark);
  if (gltang_v_is_custom(a)) {
    const GLTANG_Type * type = gltang_vm_custom(a)->type;
    if (gltang_type_has(type, op)) {
      OpCall oc = {type, op, 2, true, is_assignment};
      GLTANG_Value args[2] = {a, gltang_vm_temp_at(exec, mark + 1u)};
      GLTANG_Value r = invoke(exec, &oc, args, declined);
      if (!*declined) {
        return r;
      }
    }
  }
  // Read again: the left operand's call was a GC point.
  GLTANG_Value b = gltang_vm_temp_at(exec, mark + 1u);
  if (gltang_v_is_custom(b)) {
    const GLTANG_Type * type = gltang_vm_custom(b)->type;
    if (gltang_type_has(type, op)) {
      OpCall oc = {type, op, 2, false, is_assignment};
      GLTANG_Value args[2] = {b, gltang_vm_temp_at(exec, mark)};
      return invoke(exec, &oc, args, declined);
    }
  }
  *declined = true;
  return GLTANG_V_NULL;
}

/** Identity: the same object (a reference value's identity, AD-12), read from the temporaries. */
static bool identical(const GLTANG_Execution * exec, size_t mark) {
  GLTANG_Value a = gltang_vm_temp_at(exec, mark);
  GLTANG_Value b = gltang_vm_temp_at(exec, mark + 1u);
  return gltang_v_is_custom(a) && a == b;
}

/**
 * Whether the instruction being executed is a compound assignment's operator:
 * its operand is 1 (the compiler sets it for `+=` and the others). The
 * interpreter records the instruction's place before it calls an operation, so
 * the flag is read here, on the custom path only, and no built-in operation
 * carries it.
 */
static bool compound_assignment(const GLTANG_Execution * exec) {
  const GLTANG_Program * program = gltang_exec_current_program(exec);
  if (exec->current_function >= program->function_count) {
    return false;
  }
  const GLTANG_Function * fn = &program->functions[exec->current_function];
  if (exec->current_offset >= fn->code_count) {
    return false;
  }
  return (GLTANG_INSTRUCTION_A(fn->code[exec->current_offset]) & 1u) != 0;
}

GLTANG_Value gltang_vm_custom_binary(GLTANG_Execution * exec, GLTANG_Opcode opcode, GLTANG_Value a, GLTANG_Value b) {
  GLTANG_TypeOp op;
  if (!op_of(opcode, &op)) {
    return MAKE_ERROR(GLTANG_ERROR_NOT_SUPPORTED);
  }
  // A comparison is never an assignment (the compiler gives it operand 0).
  bool is_assignment = op <= GLTANG_TYPE_OP_MODULO && compound_assignment(exec);
  // Both operands are held across both calls, and read from the temporaries
  // after each (a call can collect, and a collection can move them).
  size_t mark = gltang_vm_temp_mark(exec);
  if (!gltang_vm_temp_push(exec, a) || !gltang_vm_temp_push(exec, b)) {
    gltang_vm_temp_release(exec, mark);
    return exec->roots[GLTANG_ROOT_OOM];
  }
  bool declined;
  GLTANG_Value r = reflect(exec, mark, op, is_assignment, &declined);
  if (declined && op == GLTANG_TYPE_OP_NOT_EQUAL) {
    // A `!=` nobody answered asks `==` the same way, and is its opposite.
    r = reflect(exec, mark, GLTANG_TYPE_OP_EQUAL, false, &declined);
    if (!declined && r != GLTANG_V_UNWIND && r != exec->roots[GLTANG_ROOT_OOM] && !gltang_vm_is_error(r)) {
      // Equal only when `equal` answers exactly true, so `!=` is that negated.
      r = gltang_v_from_bool(r != GLTANG_V_TRUE);
    }
  }
  if (declined) {
    // Every side declined: the language's own rule, made only now.
    if (op == GLTANG_TYPE_OP_EQUAL || op == GLTANG_TYPE_OP_NOT_EQUAL) {
      // Identity for two custom values; never equal to a value of another kind.
      bool same = identical(exec, mark);
      r = gltang_v_from_bool(op == GLTANG_TYPE_OP_EQUAL ? same : !same);
    }
    else {
      r = MAKE_ERROR(GLTANG_ERROR_NOT_SUPPORTED);
    }
  }
  gltang_vm_temp_release(exec, mark);
  return r;
}

GLTANG_Value gltang_vm_custom_element_equal(GLTANG_Execution * exec, GLTANG_Value a, GLTANG_Value b) {
  size_t mark = gltang_vm_temp_mark(exec);
  if (!gltang_vm_temp_push(exec, a) || !gltang_vm_temp_push(exec, b)) {
    gltang_vm_temp_release(exec, mark);
    return exec->roots[GLTANG_ROOT_OOM];
  }
  bool declined;
  GLTANG_Value r = reflect(exec, mark, GLTANG_TYPE_OP_EQUAL, false, &declined);
  if (declined) {
    r = gltang_v_from_bool(identical(exec, mark));
  }
  else if (r != GLTANG_V_UNWIND && r != exec->roots[GLTANG_ROOT_OOM]) {
    // An element is equal only when its type says so with `true`.
    r = gltang_v_from_bool(r == GLTANG_V_TRUE);
  }
  gltang_vm_temp_release(exec, mark);
  return r;
}

// ---------------------------------------------------------------------------
// The operations on one custom operand
// ---------------------------------------------------------------------------

/**
 * Calls `op` on custom `self` with `argc` operands (`self` first); the
 * language's `missing` error when the type has no such operation or declines.
 */
static GLTANG_Value unary_like(GLTANG_Execution * exec, GLTANG_TypeOp op, const GLTANG_Value * args, size_t argc, GLTANG_ErrorKind missing) {
  const GLTANG_Type * type = gltang_vm_custom(args[0])->type;
  if (!gltang_type_has(type, op)) {
    return MAKE_ERROR(missing);
  }
  OpCall oc = {type, op, argc, true, false};
  bool declined;
  GLTANG_Value r = invoke(exec, &oc, args, &declined);
  return declined ? MAKE_ERROR(missing) : r;
}

GLTANG_Value gltang_vm_custom_neg(GLTANG_Execution * exec, GLTANG_Value v) {
  GLTANG_Value args[1] = {v};
  return unary_like(exec, GLTANG_TYPE_OP_NEGATE, args, 1, GLTANG_ERROR_NOT_SUPPORTED);
}

GLTANG_Value gltang_vm_custom_attr(GLTANG_Execution * exec, GLTANG_Value v, GLTANG_Value name) {
  GLTANG_Value args[2] = {v, name};
  return unary_like(exec, GLTANG_TYPE_OP_ATTRIBUTE_GET, args, 2, GLTANG_ERROR_NOT_IMPLEMENTED);
}

GLTANG_Value gltang_vm_custom_set_attr(GLTANG_Execution * exec, GLTANG_Value v, GLTANG_Value name, GLTANG_Value x) {
  GLTANG_Value args[3] = {v, name, x};
  return unary_like(exec, GLTANG_TYPE_OP_ATTRIBUTE_SET, args, 3, GLTANG_ERROR_NOT_SUPPORTED);
}

GLTANG_Value gltang_vm_custom_index(GLTANG_Execution * exec, GLTANG_Value v, GLTANG_Value index) {
  GLTANG_Value args[2] = {v, index};
  return unary_like(exec, GLTANG_TYPE_OP_INDEX_GET, args, 2, GLTANG_ERROR_NOT_SUPPORTED);
}

GLTANG_Value gltang_vm_custom_set_index(GLTANG_Execution * exec, GLTANG_Value v, GLTANG_Value index, GLTANG_Value x) {
  GLTANG_Value args[3] = {v, index, x};
  return unary_like(exec, GLTANG_TYPE_OP_INDEX_SET, args, 3, GLTANG_ERROR_NOT_SUPPORTED);
}

GLTANG_Value gltang_vm_custom_slice(GLTANG_Execution * exec, GLTANG_Value v, GLTANG_Value start, GLTANG_Value stop, GLTANG_Value step) {
  GLTANG_Value args[4] = {v, start, stop, step};
  return unary_like(exec, GLTANG_TYPE_OP_SLICE, args, 4, GLTANG_ERROR_NOT_SUPPORTED);
}
