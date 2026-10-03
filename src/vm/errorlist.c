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
 * The error list (CAP-1; AD-13).
 *
 * Errors are values, and the language gives a program no way to see one that
 * went nowhere. The list is how the host learns of them. An error enters it
 * when it is *swallowed* and not when it is created, because the tested and
 * handled error (compared, stored, branched on) is not a problem, and only the
 * host knows whether a lost one was allowed: it is printed (and so rendered as
 * nothing), discarded by an expression statement, lost as the final value of a
 * called template, or, for a template call, the stop of its budget scope. A
 * host switch logs every error at creation instead; the halt option ends the
 * run at the first one.
 *
 * An error value cannot leave the activation that created it (a template call
 * yields text, or the limit error the calling side makes), so the chain of
 * calls above the activation where it is swallowed is the chain where it was
 * created. Each entry names the template and the chain, and the origin the
 * error carried from its creation.
 *
 * The entries are the execution's, charged to the context's allocator.
 */

#include <ghoti.io/lang-tang/macros.h>

#include <string.h>
#include "vm_internal.h"

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

const char * gltang_vm_main_name(const GLTANG_Execution * exec) {
  if (exec->name) {
    return exec->name;
  }
  const char * file = exec->program ? exec->program->file : NULL;
  return file && strcmp(file, "<program>") ? file : "main";
}

const char * gltang_vm_activation_name(const GLTANG_Execution * exec, const GLTANG_Activation * act) {
  return act->parent ? (act->name ? act->name : "?") : gltang_vm_main_name(exec);
}

// ---------------------------------------------------------------------------
// Recording
// ---------------------------------------------------------------------------

static int line_of(const GLTANG_Program * program, uint64_t function, uint64_t offset) {
  int line = 0;
  if (program && gltang_program_locate(program, function, offset, &line) != GLTANG_OK) {
    line = 0;
  }
  return line;
}

/** Makes room for one more entry; false (counted as dropped) at the cap or when memory is short. */
static GLTANG_ErrorRecord * record_begin(GLTANG_Execution * exec) {
  if (exec->error_count >= exec->error_limit) {
    ++exec->errors_dropped;
    return NULL;
  }
  if (exec->error_count == exec->error_capacity) {
    size_t capacity = exec->error_capacity ? exec->error_capacity * 2u : 16u;
    if (capacity > exec->error_limit) {
      capacity = exec->error_limit;
    }
    GLTANG_ErrorRecord * grown = gcu_allocator_realloc(exec->allocator, exec->errors, capacity * sizeof(GLTANG_ErrorRecord));
    if (!grown) {
      ++exec->errors_dropped;
      return NULL;
    }
    exec->errors = grown;
    exec->error_capacity = capacity;
  }
  GLTANG_ErrorRecord * record = &exec->errors[exec->error_count];
  memset(record, 0, sizeof(*record));
  return record;
}

/** The calls above `source`, outermost first. Returns false when memory is short. */
static bool record_chain(GLTANG_Execution * exec, GLTANG_ErrorRecord * record, const GLTANG_Activation * source) {
  size_t depth = 0;
  for (const GLTANG_Activation * a = source; a->parent; a = a->parent) {
    ++depth;
  }
  record->entry.chain_count = depth;
  if (!depth) {
    return true;
  }
  record->chain = gcu_allocator_malloc(exec->allocator, depth * sizeof(GLTANG_ErrorLink));
  if (!record->chain) {
    record->entry.chain_count = 0;
    return false;
  }
  size_t at = depth;
  for (const GLTANG_Activation * child = source; child->parent; child = child->parent) {
    const GLTANG_Activation * caller = child->parent;
    const GLTANG_Program * program = gltang_exec_program(exec, caller->program_index);
    --at;
    record->chain[at].template_name = gltang_vm_activation_name(exec, caller);
    record->chain[at].file = program ? program->file : NULL;
    record->chain[at].line = line_of(program, child->call_function, child->call_offset);
  }
  return true;
}

static void record_commit(GLTANG_Execution * exec, GLTANG_ErrorRecord * record) {
  record->entry.message = gltang_error_kind_message(record->entry.kind);
  ++exec->error_count;
}

void gltang_vm_error_swallowed(GLTANG_Execution * exec, GLTANG_ErrorHow how, GLTANG_Value error, const GLTANG_Activation * source) {
  if (!gltang_vm_is_error(error)) {
    return;
  }
  GLTANG_ErrorObject * object = gltang_object(error);
  if ((object->flags & GLTANG_ERROR_FLAG_LOGGED) || gltang_error_kind_is_marker((GLTANG_ErrorKind)object->error_kind)) {
    return;
  }
  // Entered once, whether or not the list had room: a value that was turned
  // away at the cap is counted as dropped and not offered again.
  object->flags |= GLTANG_ERROR_FLAG_LOGGED;
  GLTANG_ErrorRecord * record = record_begin(exec);
  if (!record) {
    return;
  }
  const GLTANG_Program * program = gltang_exec_program(exec, object->program);
  record->entry.kind = (GLTANG_ErrorKind)object->error_kind;
  record->entry.how = how;
  record->entry.template_name = gltang_vm_activation_name(exec, source);
  record->entry.file = program ? program->file : NULL;
  record->entry.function = object->function;
  record->entry.offset = object->offset;
  record->entry.line = line_of(program, object->function, object->offset);
  if (!record_chain(exec, record, source)) {
    ++exec->errors_dropped;
    return;
  }
  record_commit(exec, record);
}

void gltang_vm_error_scope_limit(GLTANG_Execution * exec, const GLTANG_Activation * callee) {
  GLTANG_ErrorRecord * record = record_begin(exec);
  if (!record) {
    return;
  }
  const GLTANG_Program * program = gltang_exec_program(exec, callee->program_index);
  record->entry.kind = GLTANG_ERROR_LIMIT_EXCEEDED;
  record->entry.how = GLTANG_ERROR_HOW_SCOPE_LIMIT;
  record->entry.template_name = gltang_vm_activation_name(exec, callee);
  record->entry.file = program ? program->file : NULL;
  // Where the callee was when its scope ran out.
  record->entry.function = exec->current_function;
  record->entry.offset = exec->current_offset;
  record->entry.line = line_of(program, exec->current_function, exec->current_offset);
  if (!record_chain(exec, record, callee)) {
    ++exec->errors_dropped;
    return;
  }
  record_commit(exec, record);
}

GLTANG_Value gltang_vm_make_limit_error(GLTANG_Execution * exec) {
  void * object;
  GLTANG_Status st = gltang_vm_alloc(exec, &gltang_type_error, sizeof(GLTANG_ErrorObject), &object);
  if (st != GLTANG_ST_OK) {
    return gltang_vm_failure_value(exec, st);
  }
  GLTANG_ErrorObject * error = object;
  error->kind = GLTANG_OBJ_ERROR;
  error->error_kind = GLTANG_ERROR_LIMIT_EXCEEDED;
  error->program = exec->act->program_index;
  error->function = exec->current_function;
  error->offset = exec->current_offset;
  // Entered already, as the scope limit: the call's value is not swallowed twice.
  error->flags = GLTANG_ERROR_FLAG_LOGGED;
  return gltang_value_of(error);
}

GLTANG_Value gltang_vm_error_created(GLTANG_Execution * exec, GLTANG_Value error) {
  if (!exec->log_all_errors && !exec->halt_on_error) {
    return error;
  }
  if (exec->halted) {
    return error;
  }
  gltang_vm_error_swallowed(exec, GLTANG_ERROR_HOW_CREATED, error, exec->act);
  if (!exec->halt_on_error) {
    return error;
  }
  // The first error ends the run: a keyed request that the poll turns into an
  // unwind with ERR_GUEST, taken now so that nothing more is printed.
  exec->halted = true;
  if (!exec->port || grcore_port_post(exec->port, exec->halt_kind) != GRCORE_OK) {
    return error;
  }
  if (gltang_vm_native_poll(exec, 0) != GLTANG_ST_OK) {
    return GLTANG_V_UNWIND;
  }
  return error;
}

void gltang_vm_set_result(GLTANG_Execution * exec, GLTANG_Value v, bool listed) {
  GLTANG_Activation * act = exec->act;
  if (act->result_lost) {
    // The previous statement's value was an error, and this replaces it.
    gltang_vm_error_swallowed(exec, GLTANG_ERROR_HOW_DISCARDED, act->result, act);
    act->result_lost = false;
  }
  act->result = v;
  if (listed && gltang_vm_is_error(v)) {
    const GLTANG_ErrorObject * object = gltang_object(v);
    act->result_lost = !(object->flags & GLTANG_ERROR_FLAG_LOGGED);
  }
}

void gltang_vm_errors_free(GLTANG_Execution * exec) {
  for (size_t i = 0; i < exec->error_count; ++i) {
    gcu_allocator_free(exec->allocator, exec->errors[i].chain);
  }
  gcu_allocator_free(exec->allocator, exec->errors);
  exec->errors = NULL;
  exec->error_count = exec->error_capacity = 0;
}

// ---------------------------------------------------------------------------
// The halt request
// ---------------------------------------------------------------------------

static void halt_poll(GRCORE_Context * context, void * value, GRCORE_PollCall * call) {
  (void)context;
  GLTANG_Execution * exec = value;
  // A level, re-decided at every poll: the request stays pending until the run
  // has unwound, so the vote does not depend on which handler ran first.
  if (grcore_pollcall_pending(call, exec->halt_kind)) {
    (void)grcore_pollcall_vote(call, GRCORE_VERDICT_UNWIND);
    (void)grcore_pollcall_set_unwind_result(call, GRCORE_ERR_GUEST);
  }
}

static const GRCORE_Key halt_key = {
  .name = "lang-tang halt-on-error",
  .cardinality = GRCORE_CARDINALITY_ONE,
  .phase = GRCORE_PHASE_DECIDE,
  .destroy = NULL,
  .poll = halt_poll,
};

// ---------------------------------------------------------------------------
// The host's side
// ---------------------------------------------------------------------------

static bool settable(const GLTANG_Execution * execution) {
  return execution && !execution->destroyed && !execution->in_host && execution->state == GLTANG_EXECUTION_NEW;
}

GLTANG_Result gltang_execution_set_log_all_errors(GLTANG_Execution * execution, bool enabled) {
  if (!settable(execution)) {
    return GLTANG_ERR_INVALID;
  }
  execution->log_all_errors = enabled;
  return GLTANG_OK;
}

GLTANG_Result gltang_execution_set_halt_on_error(GLTANG_Execution * execution, bool enabled) {
  if (!settable(execution)) {
    return GLTANG_ERR_INVALID;
  }
  if (enabled && !execution->halt_registered) {
    GRCORE_Context * context = execution->context;
    GRCORE_RequestKind kind;
    GRCORE_Port * port = NULL;
    GRCORE_Result r = grcore_context_request_kind(context, &halt_key, &kind);
    if (r == GRCORE_OK) {
      r = grcore_context_port(context, &port);
    }
    if (r == GRCORE_OK) {
      execution->halt_kind = kind;
      r = grcore_context_register(context, &halt_key, execution);
    }
    if (r != GRCORE_OK) {
      grcore_port_release(port);
      return r == GRCORE_ERR_OOM ? GLTANG_ERR_OOM : GLTANG_ERR_INVALID;
    }
    execution->port = port;
    execution->halt_registered = true;
  }
  execution->halt_on_error = enabled;
  return GLTANG_OK;
}

GLTANG_Result gltang_execution_set_error_limit(GLTANG_Execution * execution, size_t limit) {
  if (!settable(execution)) {
    return GLTANG_ERR_INVALID;
  }
  execution->error_limit = limit;
  return GLTANG_OK;
}

size_t gltang_execution_error_count(const GLTANG_Execution * execution) {
  return execution && !execution->destroyed ? execution->error_count : 0;
}

bool gltang_execution_error(const GLTANG_Execution * execution, size_t index, GLTANG_ErrorEntry * out_entry) {
  if (!execution || execution->destroyed || index >= execution->error_count || !out_entry) {
    return false;
  }
  *out_entry = execution->errors[index].entry;
  return true;
}

size_t gltang_execution_error_chain_count(const GLTANG_Execution * execution, size_t index) {
  return execution && !execution->destroyed && index < execution->error_count ? execution->errors[index].entry.chain_count : 0;
}

bool gltang_execution_error_chain(const GLTANG_Execution * execution, size_t index, size_t link, GLTANG_ErrorLink * out_link) {
  if (!execution || execution->destroyed || index >= execution->error_count || !out_link || link >= execution->errors[index].entry.chain_count) {
    return false;
  }
  *out_link = execution->errors[index].chain[link];
  return true;
}

uint64_t gltang_execution_errors_dropped(const GLTANG_Execution * execution) {
  return execution && !execution->destroyed ? execution->errors_dropped : 0;
}
