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
 * The bookkeeping of a template call (AD-21, AD-23): the programs a run has
 * frames of, and the record of each running program.
 *
 * A template call is a guest-to-guest call. The interpreter pushes the callee
 * program's top-level frame on the same guest stack and carries on in the same
 * loop; this file only keeps what is not on that stack. It lives in the
 * execution, not on the C stack, so a pause inside a nested template returns to
 * the host and resumes on any thread.
 */

#include <ghoti.io/lang-tang/macros.h>

#include <string.h>
#include "vm_internal.h"

bool gltang_vm_program_index(GLTANG_Execution * exec, GLTANG_Program * program, uint32_t * out_index) {
  for (size_t i = 0; i < exec->program_count; ++i) {
    if (exec->programs[i].program == program) {
      *out_index = (uint32_t)i;
      return true;
    }
  }
  if (exec->program_count == exec->program_capacity) {
    size_t capacity = exec->program_capacity * 2u;
    GLTANG_ProgramEntry * grown = gcu_allocator_realloc(exec->allocator, exec->programs, capacity * sizeof(GLTANG_ProgramEntry));
    if (!grown) {
      return false;
    }
    exec->programs = grown;
    exec->program_capacity = capacity;
  }
  size_t constants = program->constant_count ? program->constant_count : 1u;
  GLTANG_Value * cache = gcu_allocator_calloc(exec->allocator, constants, sizeof(GLTANG_Value));
  if (!cache) {
    return false;
  }
  GLTANG_ProgramEntry * entry = &exec->programs[exec->program_count];
  entry->program = gltang_program_retain(program);
  entry->constants = cache;
  *out_index = (uint32_t)exec->program_count++;
  return true;
}

GLTANG_Activation * gltang_vm_activation_new(GLTANG_Execution * exec, uint32_t program_index, const char * name) {
  GLTANG_Activation * act = gcu_allocator_calloc(exec->allocator, 1, sizeof(GLTANG_Activation));
  if (!act) {
    return NULL;
  }
  const GLTANG_Program * program = exec->programs[program_index].program;
  size_t globals = program->global_count ? program->global_count : 1u;
  act->globals = gcu_allocator_calloc(exec->allocator, globals, sizeof(GLTANG_Value));
  if (!act->globals) {
    gcu_allocator_free(exec->allocator, act);
    return NULL;
  }
  act->owns_globals = true;
  act->program_index = program_index;
  act->name = name;
  return act;
}

void gltang_vm_activation_enter(GLTANG_Execution * exec, GLTANG_Activation * act, uint32_t call_function, uint32_t call_offset) {
  act->parent = exec->act;
  act->depth = exec->act->depth + 1u;
  act->call_function = call_function;
  act->call_offset = call_offset;
  act->temp_base = exec->temp_count;
  gltang_vm_set_activation(exec, act);
}

void gltang_vm_activation_leave(GLTANG_Execution * exec, GLTANG_Activation * act) {
  gltang_vm_set_activation(exec, act->parent);
}

void gltang_vm_activation_free(GLTANG_Execution * exec, GLTANG_Activation * act) {
  if (!act) {
    return;
  }
  gltang_vm_outbuf_free(exec, &act->out);
  if (act->owns_globals) {
    gcu_allocator_free(exec->allocator, act->globals);
  }
  gcu_allocator_free(exec->allocator, act);
}
