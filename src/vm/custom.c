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
 * **A request for guest code is a status too.** A resumable operation (type.h) may answer
 * GLTANG_OP_PENDING after gltang_call_guest. The bracket then closes like any other, and what the
 * callback asked for is made into a pending record (pending.c) and handed to the interpreter by
 * returning GLTANG_V_UNWIND with `exec->request_made` set. When the callee has returned, the
 * instruction runs again: every callback of the operation whose step is before the record's is
 * skipped (it declined), and the record's step calls its continuation in a new bracket. The
 * operation's own code is therefore the step machine, run from its start each time, with no
 * second copy of it to keep in step. A site with no instruction to run again (`suspend_mode` 0)
 * runs the request as a nested activation instead.
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
#include <ghoti.io/cutil/memory.h>
#include "vm_internal.h"
#ifdef GLTANG_WITH_JIT
#include "../jit/jit.h"
#include "../jit/jit_internal.h"
#endif

#define MAKE_ERROR(kind) gltang_vm_make_error(exec, (kind))

/**
 * The most operands one operation is held in this C frame: `self` and up to
 * sixteen more (a call's arguments, as a native's, are copied to the heap past
 * that). A slice has the most of the others, four.
 */
#define OPERANDS_MAX 17u

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

/** Whether `incoming` holds an attribute of a built-in kind that an attached registry holds. */
static bool attribute_clashes(const GLTANG_HandleState * hs, const GLTANG_TypeRegistry * incoming) {
  for (size_t r = 0; r < hs->registry_count; ++r) {
    for (size_t i = 0; i < incoming->attribute_count; ++i) {
      const GLTANG_HostAttribute * a = &incoming->attributes[i];
      if (gltang_type_registry_find_attribute(hs->registries[r], a->kind, a->name, a->name_length)) {
        return true;
      }
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
  if (clashes(hs, registry) || attribute_clashes(hs, registry)) {
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
  _Static_assert(GLTANG_TYPE_SLOTS_MAX <= UINT16_MAX, "an instance's slot count is kept in 16 bits");
  custom->slot_count = (uint16_t)slots;
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

/**
 * What only some operations need, kept apart so that the common ones (a binary operator, an
 * attribute, an index) set one pointer for it and not four members.
 */
typedef struct OpExtra {
  GLTANG_CastTarget target;                 ///< CAST only.
  const GLTANG_HostAttribute * attribute;   ///< A host attribute of a built-in kind, not an operation of a type (op GLTANG_TYPE_OP_COUNT).
  bool * done;                              ///< ITERATOR_NEXT only: set when the callback answered GLTANG_OP_DONE (no value is made).
  bool * errored;                           ///< Set when the callback did not answer GLTANG_OP_OK (the value is then an error, the out-of-memory value or an unwind).
} OpExtra;

/** An operation's operands and flags, as they are handed to the callback. */
typedef struct OpCall {
  const GLTANG_Type * type;
  GLTANG_TypeOp op;
  size_t argc;
  bool self_is_lhs;
  bool is_assignment;
  const OpExtra * extra;                    ///< NULL for an operation that needs none.
} OpCall;

#define OPCALL(type_, op_, argc_, lhs_, assign_) \
  {(type_), (op_), (argc_), (lhs_), (assign_), NULL}

/** Calls the callback with the call object and the handles made for `args`. */
static GLTANG_OpStatus run_callback(GLTANG_NativeCall * call, const OpCall * oc, const GLTANG_Handle * h) {
  if (oc->op == GLTANG_TYPE_OP_COUNT) {
    // A host attribute of a built-in kind: no type, and no operation of one.
    return oc->extra->attribute->fn(call, oc->extra->attribute->user, h[0]);
  }
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
    case GLTANG_TYPE_OP_ITERATOR_BEGIN: return d->iterator_begin(call, user, h[0]);
    case GLTANG_TYPE_OP_ITERATOR_NEXT: return d->iterator_next(call, user, h[0]);
    case GLTANG_TYPE_OP_CALL: return d->call(call, user, h[0], h + 1, oc->argc - 1u);
    case GLTANG_TYPE_OP_CAST: return d->cast(call, user, h[0], oc->extra->target);
    case GLTANG_TYPE_OP_TRUTHY: {
      bool truthy = true;
      GLTANG_OpStatus status = d->truthy(call, user, h[0], &truthy);
      if (status == GLTANG_OP_OK && call->set != GLTANG_CALL_ERROR) {
        // The answer is the boolean the callback left in its out parameter, unless it set an
        // error through the call object, which is the call's value whatever it returned.
        gltang_call_return_bool(call, truthy);
      }
      return status;
    }
    case GLTANG_TYPE_OP_PRINT: return d->print(call, user, h[0]);
    case GLTANG_TYPE_OP_TO_STRING: return d->to_string(call, user, h[0]);
    case GLTANG_TYPE_OP_COUNT: break;
  }
  return GLTANG_OP_ERROR;
}

/**
 * What one bracket is entered with and leaves: the continuation to call in place of the callback,
 * and, when the callback asked for a guest call at a site that cannot suspend, the call to make
 * (the callee, its arguments, the continuation and the state it names).
 */
typedef struct Stage {
  bool pop;                     ///< In: the values came from the innermost pending record, which is let go once they are held.
  uint32_t index;
  GLTANG_Value state;
  GLTANG_Value result;
  bool nested;                  ///< Out: run `callee` as a nested activation, then enter again with the continuation `index`.
  GLTANG_Value callee;
  size_t argc;
  GLTANG_Value args[GLTANG_GUEST_CALL_ARGS_MAX];
} Stage;

/** The continuation of the type's table, called as the callback is. */
static GLTANG_OpStatus run_continuation(GLTANG_NativeCall * call, const OpCall * oc, uint32_t index, GLTANG_Handle state, GLTANG_Handle result) {
  const GLTANG_Type * type = oc->type;
  return type->continuations[index].function(call, type->descriptor.user, state, result);
}

/**
 * One bracket: the callback (or a continuation), under a record of its own, with a scope of its
 * own, until it returns. A GC point.
 *
 * A callback that asked for a guest call and returned GLTANG_OP_PENDING ends here all the same:
 * its scope is closed, and what it asked for is taken out of the handles (bracket_request). At an
 * instruction site (`suspend_mode`) the request becomes a pending record, the record of this
 * bracket is left, and GLTANG_V_UNWIND is returned with `request_made` set: the interpreter starts
 * the callee from its own loop, with nothing of this call on the C stack. At any other site
 * `stage->nested` is set and the caller runs the call. A callback that asks for nothing pays for
 * one load and one test.
 */

static GLTANG_Value invoke_nested(GLTANG_Execution * exec, const OpCall * oc, const GLTANG_Value * args, bool * declined, uint32_t step, Stage * stage);

/**
 * The cold half of a bracket: the callback returned with a request recorded (`hs->request.made`). A request
 * is accepted only from a callback that returned GLTANG_OP_PENDING and whose handles were all good; any other
 * status after a request is `Host function failed` (the call's error is set and `*status` becomes ERROR).
 * When it is accepted the callback is closed (its scope), the request is held in the temporaries and made
 * into a pending record (or, at a site that cannot suspend, handed to the caller in `stage`), `*result` is
 * the operation's value, and true is returned: nothing is left to settle. Otherwise false.
 */
static bool __attribute__((noinline)) bracket_request(GLTANG_Execution * exec, const OpCall * oc, GLTANG_NativeCall * call, GLTANG_OpStatus * status, const GLTANG_Value * held, const GLTANG_Value * args,
    uint32_t step, bool * declined, Stage * stage, GLTANG_Value * result) {
  GLTANG_HandleState * const hs = exec->handles;
  if (call->guest_refused) {
    // A request was refused (gltang_call_guest set the error): the call failed, whatever status it returned (a
    // decline or a DONE included), and a request it made before that does not stand either.
    hs->request.made = false;
    gltang_call_return_error(call, GLTANG_ERROR_HOST_FAILED);
    *status = GLTANG_OP_ERROR;
    return false;
  }
  if (*status != GLTANG_OP_PENDING) {
    hs->request.made = false;
    gltang_call_return_error(call, GLTANG_ERROR_HOST_FAILED);
    *status = GLTANG_OP_ERROR;
    return false;
  }
  if (call->scope_unwind || call->scope_oom || call->set == GLTANG_CALL_ERROR) {
    hs->request.made = false;
    return false;
  }
  // Close the callback, keeping what it asked for in the temporaries (the collector updates them) until the
  // pending record, or the nested call, has it.
  GLTANG_GuestRequest * request = &hs->request;
  const uint32_t request_argc = request->argc;
  const uint32_t request_index = request->continuation;
  GLTANG_Value values[2u + GLTANG_GUEST_CALL_ARGS_MAX];
  values[0] = request->state;
  values[1] = request->callee;
  for (uint32_t i = 0; i < request_argc; ++i) {
    values[2u + i] = request->args[i];
  }
  request->made = false;
  gltang_vm_call_abandon(call);
  *declined = false;
  const size_t request_base = gltang_vm_temp_mark(exec);
  bool held_request = true;
  for (uint32_t i = 0; i < 2u + request_argc && held_request; ++i) {
    held_request = gltang_vm_temp_push(exec, values[i]);
  }
  if (!held_request) {
    *result = exec->roots[GLTANG_ROOT_OOM];
    if (oc->extra && oc->extra->errored) {
      *oc->extra->errored = true;
    }
  }
  else if (exec->suspend_mode != 0) {
    GLTANG_PendingSpec spec = {
      .type = oc->type, .member = NULL, .op = oc->op, .step = step, .continuation = request_index,
      .chained = exec->suspend_mode == 2u, .temp_base = request_base, .in_temps = true, .argc = request_argc,
      .operands = exec->suspend_mode == 2u ? held : NULL, .operand_count = exec->suspend_mode == 2u ? oc->argc : 0u,
    };
    *result = gltang_vm_pending_push(exec, &spec);
    if (*result == GLTANG_V_UNWIND || *result == exec->roots[GLTANG_ROOT_OOM]) {
      if (oc->extra && oc->extra->errored) {
        *oc->extra->errored = true;
      }
    }
    else {
      // The record is the innermost; the callee is started from the interpreter's own loop.
      ++hs->suspensions;
      exec->request_made = true;
      *result = GLTANG_V_UNWIND;
    }
  }
  else {
    // A site inside a C loop has no instruction to run again: the call is made nested (AD-23). A first entry
    // makes it here, and the continuation's brackets hand a further request back to the loop (`stage`).
    Stage local;
    Stage * nested = stage ? stage : &local;
    nested->nested = true;
    nested->state = gltang_vm_temp_at(exec, request_base);
    nested->callee = gltang_vm_temp_at(exec, request_base + 1u);
    nested->argc = request_argc;
    for (uint32_t i = 0; i < request_argc; ++i) {
      nested->args[i] = gltang_vm_temp_at(exec, request_base + 2u + i);
    }
    nested->index = request_index;
    if (!stage) {
      *result = invoke_nested(exec, oc, args, declined, step, &local);
    }
  }
  gltang_vm_temp_release(exec, request_base);
  return true;
}

static inline __attribute__((always_inline)) GLTANG_Value bracket_impl(GLTANG_Execution * exec, const OpCall * oc, const GLTANG_Value * args, bool * declined, uint32_t step, Stage * stage, const bool continuing) {
  if (stage) {
    stage->nested = false;
  }
  // The operands are copied into this frame first; the record gives exactly the
  // copies as its conservative segment, so the objects they name are pinned and
  // the callback reads the copies (as gltang_vm_call_native does). More than a
  // frame holds (a call with many arguments) are copied to the heap, zeroed.
  GLTANG_Value held_small[OPERANDS_MAX];
  GLTANG_Handle h_small[OPERANDS_MAX];
  GLTANG_Value * held = held_small;
  GLTANG_Handle * h = h_small;
  if (oc->argc > OPERANDS_MAX) {
    held = gcu_calloc(oc->argc, sizeof(GLTANG_Value));
    h = gcu_calloc(oc->argc, sizeof(GLTANG_Handle));
    if (!held || !h) {
      gcu_free(held);
      gcu_free(h);
      if (oc->extra && oc->extra->errored) {
        *oc->extra->errored = true;
      }
      if (continuing && stage->pop) {
        // A first entry (bracket_entry) has no stage: only a continuation took a record.
        gltang_vm_pending_pop(exec);
        exec->resume_active = false;
      }
      return exec->roots[GLTANG_ROOT_OOM];
    }
  }
  // One or two operands are nearly every operation: copied in place, with no loop or call.
  switch (oc->argc) {
    case 1:
      held[0] = args[0];
      break;
    case 2:
      held[0] = args[0];
      held[1] = args[1];
      break;
    default:
      for (size_t i = 0; i < oc->argc; ++i) {
        held[i] = args[i];
      }
      break;
  }
  // A continuation's state and result wait in the temporaries (which the collector updates) from here until the
  // handles are made; the record they came from has done its work.
  size_t hold = 0;
  if (continuing) {
    hold = gltang_vm_temp_mark(exec);
    const bool held_both = gltang_vm_temp_push(exec, stage->state) && gltang_vm_temp_push(exec, stage->result);
    if (stage->pop) {
      gltang_vm_pending_pop(exec);
      exec->resume_active = false;
    }
    if (!held_both) {
      gltang_vm_temp_release(exec, hold);
      if (held != held_small) {
        gcu_free(held);
        gcu_free(h);
      }
      if (oc->extra && oc->extra->errored) {
        *oc->extra->errored = true;
      }
      return exec->roots[GLTANG_ROOT_OOM];
    }
  }
  // Charged as a native call; the bytes of a string answer are charged below.
  exec->pending_fuel += gltang_opcode_cost_table[GLTANG_OP_CALL];
#ifdef GLTANG_WITH_JIT
  // From a helper that compiled code called (a cast, a print, a host attribute of a string), the
  // native-depth units its open records hold are handed back for the call, as for a native
  // (gltang_vm_call_native), so the same budget refuses at the same depth in both tiers. With no
  // compiled record open, as in the interpreter, it lends nothing.
  // The test is made here, inline: it is false in the interpreter and for every operation reached
  // from it, which must not pay a call for it.
  const size_t lent = exec->jit && exec->jit->records_open > exec->jit->depth_lent ? gltang_jit_lend_depth(exec) : 0;
#endif
  GRCORE_Stack * stack = grcore_context_stack(exec->context);
  GRCORE_CSegment segment = {(uintptr_t)&held[0], (uintptr_t)&held[0] + oc->argc * sizeof(GLTANG_Value)};
  GRCORE_ActivationRef record;
  GRCORE_Result entered = grcore_activation_enter(stack, GRCORE_ACTIVATION_NATIVE, exec->engine, false, &segment, &record);
  if (entered != GRCORE_OK) {
    if (continuing) {
      gltang_vm_temp_release(exec, hold);
    }
    if (held != held_small) {
      gcu_free(held);
      gcu_free(h);
    }
    if (oc->extra && oc->extra->errored) {
      *oc->extra->errored = true;
    }
    GLTANG_Value refused = gltang_vm_native_refused_value(exec, entered);
#ifdef GLTANG_WITH_JIT
    if (lent) {
      gltang_jit_return_depth(exec, lent);
    }
#endif
    return refused;
  }
  if (exec->native_gc_seam) {
    // Entering a nested activation is a GC point (AD-17), made here under torture.
    (void)grheap_collect(exec->heap);
  }
  GLTANG_NativeCall call;
  memset(&call, 0, GLTANG_CALL_ZEROED);
  call.exec = exec;
  call.args = held;
  call.argc = oc->argc;
  // What decides whether this callback may ask for a guest call (gltang_call_guest): the type's list. The attribute
  // of a built-in kind has no type, and never may.
  call.owner = oc->type;
  call.op = (uint8_t)oc->op;
  bool made = true;
  for (size_t i = 0; i < oc->argc && made; ++i) {
    made = gltang_call_arg(&call, i, &h[i]) == GLTANG_OK;
  }
  GLTANG_Handle h_state = {0};
  GLTANG_Handle h_result = {0};
  if (continuing) {
    call.resume_step = step;
    // Read from the temporaries, which the collector kept up to date through the seam above.
    made = made && gltang_vm_handle_push(&call, gltang_vm_temp_at(exec, hold), &h_state) == GLTANG_OK && gltang_vm_handle_push(&call, gltang_vm_temp_at(exec, hold + 1u), &h_result) == GLTANG_OK;
  }
  GLTANG_OpStatus status = GLTANG_OP_ERROR;
  if (made) {
    // Host code from here to its return: it cannot destroy or reconfigure the
    // execution (the setters refuse), as in a native.
    const bool outer_in_host = exec->in_host;
    exec->in_host = true;
    status = continuing ? run_continuation(&call, oc, stage->index, h_state, h_result) : run_callback(&call, oc, h);
    exec->in_host = outer_in_host;
  }
  GLTANG_Value result = GLTANG_V_NULL;
  bool settle = true;
  // A callback that asked for a guest call (gltang_call_guest): the cold half takes it from here.
  if (__builtin_expect(call.requested, 0) && bracket_request(exec, oc, &call, &status, held, args, step, declined, stage, &result)) {
    settle = false;
  }
  else if (status != GLTANG_OP_OK && !call.scope_unwind && !call.scope_oom) {
    if (status == GLTANG_OP_DECLINE) {
      // A decline: whatever the callback set is dropped, and no value is made.
      gltang_vm_call_abandon(&call);
      *declined = true;
      result = GLTANG_V_NULL;
      settle = false;
    }
    else if (status == GLTANG_OP_DONE && oc->extra && oc->extra->done) {
      // An iterator has no more items: a status like a decline, and nothing is made.
      gltang_vm_call_abandon(&call);
      *oc->extra->done = true;
      result = GLTANG_V_NULL;
      settle = false;
    }
  }
  if (settle) {
    if (status == GLTANG_OP_OK) {
      if (call.set == GLTANG_CALL_NONE && (oc->op == GLTANG_TYPE_OP_ATTRIBUTE_SET || oc->op == GLTANG_TYPE_OP_INDEX_SET)) {
        // An assignment with no answer of its own is worth the value assigned.
        call.set = GLTANG_CALL_VALUE;
        call.value = held[2];
      }
    }
    else if (status == GLTANG_OP_DONE || call.set != GLTANG_CALL_ERROR) {
      // An error with no kind set, or a status this engine does not know (a PENDING that asked for
      // nothing included); and a DONE, which is valid from an iterator's `iterator_next` only
      // (above), whatever kind it set.
      gltang_call_return_error(&call, GLTANG_ERROR_HOST_FAILED);
    }
    // Read before the call settles: an error set through the call object is the value whatever the
    // status was, and the caller must not take that value for an answer.
    const bool set_error = call.set == GLTANG_CALL_ERROR;
    result = gltang_vm_call_settle(exec, &call, true);
    if (oc->extra && oc->extra->errored) {
      *oc->extra->errored = status != GLTANG_OP_OK || set_error || call.scope_oom || call.scope_unwind;
    }
    if (gltang_v_is_kind(result, GLTANG_OBJ_STRING)) {
      // A callback costs fuel in proportion to the bytes it answers, as a native does.
      exec->pending_fuel += gltang_vm_string(result)->byte_length / GLTANG_WORK_BYTES_PER_FUEL;
    }
  }
  (void)grcore_activation_leave(stack, record);
#ifdef GLTANG_WITH_JIT
  if (lent) {
    gltang_jit_return_depth(exec, lent);
  }
#endif
  if (held != held_small) {
    gcu_free(held);
    gcu_free(h);
  }
  if (continuing) {
    gltang_vm_temp_release(exec, hold);
  }
  return result;
}

/** The bracket of a first entry: the continuation's parts are compiled out, and a request made at a site that cannot suspend is run nested inside it. */
static inline __attribute__((always_inline)) GLTANG_Value bracket_entry(GLTANG_Execution * exec, const OpCall * oc, const GLTANG_Value * args, bool * declined, uint32_t step) {
  return bracket_impl(exec, oc, args, declined, step, NULL, false);
}

/** The bracket of a continuation, out of line: the common entry does not carry it. */
static GLTANG_Value __attribute__((noinline)) bracket_continue(GLTANG_Execution * exec, const OpCall * oc, const GLTANG_Value * args, bool * declined, uint32_t step, Stage * stage) {
  return bracket_impl(exec, oc, args, declined, step, stage, true);
}

/**
 * The cold half of `invoke` at a site that cannot suspend: the callback (or a continuation) asked for a guest call
 * and the call is made here, as a nested activation (AD-23), which is how an opaque host function would re-enter.
 * Only a Tang function is accepted, and only while the program runs; a pause inside is an unwind that stops at
 * the nested record. The state is held across it, and the answer until the next bracket has copied it. `r` is the
 * bracket's result, which the request left unused; a continuation that asks again is made the same way.
 */
static GLTANG_Value __attribute__((noinline)) invoke_nested(GLTANG_Execution * exec, const OpCall * oc, const GLTANG_Value * args, bool * declined, uint32_t step, Stage * stage) {
  size_t mark = SIZE_MAX;
  for (;;) {
    ++exec->handles->nested_requests;
    if (exec->state != GLTANG_EXECUTION_RUNNING || !gltang_v_is_function(stage->callee)) {
      // Only a Tang function, and only while the program runs: a host rendering the result of a finished or
      // paused run has no frames to nest one on.
      if (mark != SIZE_MAX) {
        gltang_vm_temp_release(exec, mark);
      }
      return MAKE_ERROR(GLTANG_ERROR_HOST_FAILED);
    }
    if (mark == SIZE_MAX) {
      mark = gltang_vm_temp_mark(exec);
    }
    else {
      gltang_vm_temp_release(exec, mark);
    }
    if (!gltang_vm_temp_push(exec, stage->state)) {
      gltang_vm_temp_release(exec, mark);
      return exec->roots[GLTANG_ROOT_OOM];
    }
    GLTANG_Value answer = gltang_vm_call_guest(exec, stage->callee, stage->argc, stage->args);
    if (answer == GLTANG_V_UNWIND || !gltang_vm_temp_push(exec, answer)) {
      GLTANG_Value failed = answer == GLTANG_V_UNWIND ? GLTANG_V_UNWIND : exec->roots[GLTANG_ROOT_OOM];
      gltang_vm_temp_release(exec, mark);
      return failed;
    }
    stage->pop = false;
    stage->nested = false;
    stage->state = gltang_vm_temp_at(exec, mark);
    stage->result = gltang_vm_temp_at(exec, mark + 1u);
    GLTANG_Value r = bracket_continue(exec, oc, args, declined, step, stage);
    if (!stage->nested) {
      gltang_vm_temp_release(exec, mark);
      return r;
    }
    // The continuation asked again: the loop makes that call too.
  }
}

/**
 * The cold half of `invoke` when an instruction is run again to continue from a pending record: the steps before
 * the record's were declines and are not called again, the record's step calls its continuation, and a later step
 * (or a record that is not this operation's) is not reached by a correct replay. Returns true when `*early` is
 * the operation's answer (or, with `*declined`, a decline), false when the bracket is to run with `stage` set up.
 */
static bool __attribute__((noinline)) invoke_resume(GLTANG_Execution * exec, const OpCall * oc, uint32_t step, Stage * stage, bool * declined, GLTANG_Value * early) {
  const GLTANG_PendingObject * record = gltang_object(exec->pending_top);
  if (step < record->step) {
    // An earlier step of the operation: it declined, or the operation would not be here.
    *declined = true;
    *early = GLTANG_V_NULL;
    return true;
  }
  if (step > record->step || record->type != oc->type || record->op != (uint8_t)oc->op) {
    // Not reached by a correct replay. The operation's value is the failure; nothing runs.
    gltang_vm_pending_pop(exec);
    exec->resume_active = false;
    *early = MAKE_ERROR(GLTANG_ERROR_HOST_FAILED);
    return true;
  }
  stage->pop = true;
  stage->index = record->continuation;
  stage->state = record->slots[GLTANG_PENDING_STATE];
  stage->result = record->slots[GLTANG_PENDING_RESULT];
  return false;
}

/**
 * Calls one operation of a type. `args` are the operands, `self` first. The
 * answer is the value; `*declined` says the callback declined, and then the
 * value is null and nothing was made. A GC point.
 *
 * `step` is the callback's place in the instruction's dispatch (the table in design.md): when an
 * instruction runs again to continue from a pending record, the steps before the record's were
 * declines and are not called again, and the record's step calls its continuation. A callback that
 * asked for a guest call at an instruction site answers GLTANG_V_UNWIND with `exec->request_made`
 * set (every caller already hands an unwind up unchanged).
 */
static GLTANG_Value invoke(GLTANG_Execution * exec, const OpCall * oc, const GLTANG_Value * args, bool * declined, uint32_t step) {
  *declined = false;
  if (__builtin_expect(exec->resume_active, 0)) {
    Stage stage;
    GLTANG_Value early;
    if (invoke_resume(exec, oc, step, &stage, declined, &early)) {
      return early;
    }
    GLTANG_Value r = bracket_continue(exec, oc, args, declined, step, &stage);
    return stage.nested ? invoke_nested(exec, oc, args, declined, step, &stage) : r;
  }
  return bracket_entry(exec, oc, args, declined, step);
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
static GLTANG_Value reflect(GLTANG_Execution * exec, size_t mark, GLTANG_TypeOp op, bool is_assignment, uint32_t base_step, bool * declined) {
  *declined = true;
  GLTANG_Value a = gltang_vm_temp_at(exec, mark);
  if (gltang_v_is_custom(a)) {
    const GLTANG_Type * type = gltang_vm_custom(a)->type;
    if (gltang_type_has(type, op)) {
      OpCall oc = OPCALL(type, op, 2, true, is_assignment);
      GLTANG_Value args[2] = {a, gltang_vm_temp_at(exec, mark + 1u)};
      GLTANG_Value r = invoke(exec, &oc, args, declined, base_step);
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
      OpCall oc = OPCALL(type, op, 2, false, is_assignment);
      GLTANG_Value args[2] = {b, gltang_vm_temp_at(exec, mark)};
      return invoke(exec, &oc, args, declined, base_step + 1u);
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
  if (op != GLTANG_TYPE_OP_EQUAL && op != GLTANG_TYPE_OP_NOT_EQUAL) {
    // An error operand is the value, and the type is not asked, as a string's
    // `+` gives its error operand (a custom value is never an error, so at
    // most one side is). Equality keeps its rule: it is asked of any pair.
    if (gltang_vm_stands_for_error(a) || gltang_vm_stands_for_error(b)) {
      if (exec->resume_active) {
        // Run again to continue from a pending record, and the guest call marked an operand as an error
        // in between: the rule answers before any callback, so the record has nothing to continue.
        gltang_vm_pending_pop(exec);
        exec->resume_active = false;
      }
      return gltang_vm_stands_for_error(a) ? a : b;
    }
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
  GLTANG_Value r = reflect(exec, mark, op, is_assignment, 0, &declined);
  if (r == GLTANG_V_UNWIND) {
    // Failed, or the left or right operand's callback asked for a guest call (a pending record
    // holds the rest): either way the operation goes no further here.
    gltang_vm_temp_release(exec, mark);
    return r;
  }
  if (declined && op == GLTANG_TYPE_OP_NOT_EQUAL) {
    // A `!=` nobody answered asks `==` the same way, and is its opposite (steps 2 and 3).
    r = reflect(exec, mark, GLTANG_TYPE_OP_EQUAL, false, 2, &declined);
    if (r == GLTANG_V_UNWIND) {
      gltang_vm_temp_release(exec, mark);
      return r;
    }
    if (!declined && r != GLTANG_V_UNWIND && r != exec->roots[GLTANG_ROOT_OOM] && !gltang_vm_stands_for_error(r)) {
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
  GLTANG_Value r = reflect(exec, mark, GLTANG_TYPE_OP_EQUAL, false, 0, &declined);
  if (declined) {
    r = gltang_v_from_bool(identical(exec, mark));
  }
  else if (r != GLTANG_V_UNWIND && r != exec->roots[GLTANG_ROOT_OOM] && !gltang_vm_stands_for_error(r)) {
    // An element is equal only when its type says so with `true`; an error it
    // answers is the comparison's value, as at the top level.
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
  OpCall oc = OPCALL(type, op, argc, true, false);
  bool declined;
  GLTANG_Value r = invoke(exec, &oc, args, &declined, 0);
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

// ---------------------------------------------------------------------------
// Iteration, calls, casts, truthiness, text (story 6)
// ---------------------------------------------------------------------------
//
// Each is one more operation of the one bracket above. A decline is a status,
// and the default after it is exactly what a type with no entry gets.

GLTANG_Value gltang_vm_custom_iterator_begin(GLTANG_Execution * exec, GLTANG_Value v) {
  const GLTANG_Type * type = gltang_vm_custom(v)->type;
  if (!gltang_type_has(type, GLTANG_TYPE_OP_ITERATOR_BEGIN)) {
    return MAKE_ERROR(GLTANG_ERROR_NOT_SUPPORTED);
  }
  OpCall oc = OPCALL(type, GLTANG_TYPE_OP_ITERATOR_BEGIN, 1, true, false);
  GLTANG_Value args[1] = {v};
  bool declined;
  GLTANG_Value r = invoke(exec, &oc, args, &declined, 0);
  if (declined) {
    return MAKE_ERROR(GLTANG_ERROR_NOT_SUPPORTED);
  }
  if (r == GLTANG_V_UNWIND || gltang_vm_is_error(r)) {
    // A failure of the call, or the error the type answered: the loop's value.
    return r;
  }
  if (gltang_v_is_kind(r, GLTANG_OBJ_ARRAY)) {
    return r;
  }
  if (gltang_v_is_custom(r) && gltang_type_has(gltang_vm_custom(r)->type, GLTANG_TYPE_OP_ITERATOR_NEXT)) {
    return r;
  }
  return MAKE_ERROR(GLTANG_ERROR_NOT_SUPPORTED);
}

GLTANG_IterStep gltang_vm_custom_iterator_next(GLTANG_Execution * exec, GLTANG_Value it, GLTANG_Value * out) {
  const GLTANG_Type * type = gltang_vm_custom(it)->type;
  GLTANG_Value r;
  bool declined = !gltang_type_has(type, GLTANG_TYPE_OP_ITERATOR_NEXT);
  bool done = false;
  bool errored = false;
  if (!declined) {
    OpCall oc = OPCALL(type, GLTANG_TYPE_OP_ITERATOR_NEXT, 1, true, false);
    OpExtra extra = {GLTANG_CAST_TARGET_INT, NULL, &done, &errored};
    oc.extra = &extra;
    GLTANG_Value args[1] = {it};
    r = invoke(exec, &oc, args, &declined, 0);
  }
  else {
    r = GLTANG_V_NULL;
  }
  if (declined) {
    // The language's error, made now that the type has nothing to say.
    r = MAKE_ERROR(GLTANG_ERROR_NOT_SUPPORTED);
    *out = r;
    return r == GLTANG_V_UNWIND ? GLTANG_ITER_UNWIND : GLTANG_ITER_ERROR;
  }
  if (done) {
    return GLTANG_ITER_DONE;
  }
  *out = r;
  if (r == GLTANG_V_UNWIND) {
    return GLTANG_ITER_UNWIND;
  }
  return errored ? GLTANG_ITER_ERROR : GLTANG_ITER_ITEM;
}

GLTANG_Value gltang_vm_custom_call(GLTANG_Execution * exec, const GLTANG_Value * callee_and_args, size_t argc) {
  const GLTANG_Type * type = gltang_vm_custom(callee_and_args[0])->type;
  if (!gltang_type_has(type, GLTANG_TYPE_OP_CALL)) {
    return MAKE_ERROR(GLTANG_ERROR_INVALID_FUNCTION_CALL);
  }
  OpCall oc = OPCALL(type, GLTANG_TYPE_OP_CALL, argc + 1u, true, false);
  bool declined;
  GLTANG_Value r = invoke(exec, &oc, callee_and_args, &declined, 0);
  return declined ? MAKE_ERROR(GLTANG_ERROR_INVALID_FUNCTION_CALL) : r;
}

GLTANG_Value gltang_vm_custom_truth(GLTANG_Execution * exec, GLTANG_Value v) {
  const GLTANG_CustomObject * custom = gltang_vm_custom(v);
  if (custom->flags & GLTANG_CUSTOM_FLAG_ERROR) {
    // An instance that stands for an error is falsy, and its type is not asked.
    return GLTANG_V_FALSE;
  }
  if (!gltang_type_has(custom->type, GLTANG_TYPE_OP_TRUTHY)) {
    return GLTANG_V_TRUE;
  }
  OpCall oc = OPCALL(custom->type, GLTANG_TYPE_OP_TRUTHY, 1, true, false);
  GLTANG_Value args[1] = {v};
  bool declined;
  GLTANG_Value r = invoke(exec, &oc, args, &declined, 0);
  return declined ? GLTANG_V_TRUE : r;
}

GLTANG_Value gltang_vm_custom_cast(GLTANG_Execution * exec, GLTANG_Value v, GLTANG_CastTarget target, bool * declined) {
  const GLTANG_Type * type = gltang_vm_custom(v)->type;
  if (!gltang_type_has(type, GLTANG_TYPE_OP_CAST)) {
    *declined = true;
    return GLTANG_V_NULL;
  }
  bool errored = false;
  OpCall oc = OPCALL(type, GLTANG_TYPE_OP_CAST, 1, true, false);
  OpExtra extra = {target, NULL, NULL, &errored};
  oc.extra = &extra;
  GLTANG_Value args[1] = {v};
  GLTANG_Value r = invoke(exec, &oc, args, declined, 0);
  if (*declined || errored || r == GLTANG_V_UNWIND || gltang_vm_is_error(r)) {
    return r;
  }
  // An answer is of the target's kind (a type answering an integer to `as bool` is wrong, as an
  // answer of `print` that is not a string is): else the call failed.
  GLTANG_ValueKind want = GLTANG_KIND_INTEGER;
  switch (target) {
    case GLTANG_CAST_TARGET_INT: want = GLTANG_KIND_INTEGER; break;
    case GLTANG_CAST_TARGET_FLOAT: want = GLTANG_KIND_FLOAT; break;
    case GLTANG_CAST_TARGET_BOOL: want = GLTANG_KIND_BOOL; break;
    case GLTANG_CAST_TARGET_STRING: want = GLTANG_KIND_STRING; break;
  }
  return gltang_vm_kind(r) == want ? r : MAKE_ERROR(GLTANG_ERROR_HOST_FAILED);
}

/** The answer of a `print` or `to_string`: a string, or the failure; anything else is `Host function failed`. */
static GLTANG_Value text_answer(GLTANG_Execution * exec, GLTANG_Value r, bool errored) {
  if (errored || r == GLTANG_V_UNWIND || gltang_v_is_kind(r, GLTANG_OBJ_STRING)) {
    return r;
  }
  return MAKE_ERROR(GLTANG_ERROR_HOST_FAILED);
}

GLTANG_Value gltang_vm_custom_text(GLTANG_Execution * exec, GLTANG_Value v, bool for_print, bool * declined) {
  const GLTANG_Type * type = gltang_vm_custom(v)->type;
  GLTANG_Value args[1] = {v};
  *declined = true;
  if (for_print && gltang_type_has(type, GLTANG_TYPE_OP_PRINT)) {
    bool errored = false;
    OpCall oc = OPCALL(type, GLTANG_TYPE_OP_PRINT, 1, true, false);
    OpExtra extra = {GLTANG_CAST_TARGET_INT, NULL, NULL, &errored};
    oc.extra = &extra;
    GLTANG_Value r = invoke(exec, &oc, args, declined, 0);
    if (!*declined) {
      return text_answer(exec, r, errored);
    }
  }
  if (gltang_type_has(type, GLTANG_TYPE_OP_TO_STRING)) {
    bool errored = false;
    OpCall oc = OPCALL(type, GLTANG_TYPE_OP_TO_STRING, 1, true, false);
    OpExtra extra = {GLTANG_CAST_TARGET_INT, NULL, NULL, &errored};
    oc.extra = &extra;
    GLTANG_Value r = invoke(exec, &oc, args, declined, 1);
    if (!*declined) {
      return text_answer(exec, r, errored);
    }
  }
  *declined = true;
  return GLTANG_V_NULL;
}

GLTANG_Value gltang_vm_host_attribute(GLTANG_Execution * exec, GLTANG_Value container, const char * name, size_t length, bool * found) {
  *found = false;
  const GLTANG_HandleState * hs = exec->handles;
  if (!hs) {
    return GLTANG_V_NULL;
  }
  const GLTANG_ValueKind kind = gltang_vm_kind(container);
  for (size_t i = 0; i < hs->registry_count; ++i) {
    const GLTANG_HostAttribute * attribute = gltang_type_registry_find_attribute(hs->registries[i], kind, name, length);
    if (!attribute) {
      continue;
    }
    *found = true;
    OpCall oc = OPCALL(NULL, GLTANG_TYPE_OP_COUNT, 1, true, false);
    OpExtra extra = {GLTANG_CAST_TARGET_INT, attribute, NULL, NULL};
    oc.extra = &extra;
    GLTANG_Value args[1] = {container};
    bool declined;
    GLTANG_Value r = invoke(exec, &oc, args, &declined, 0);
    return declined ? MAKE_ERROR(GLTANG_ERROR_NOT_IMPLEMENTED) : r;
  }
  return GLTANG_V_NULL;
}
