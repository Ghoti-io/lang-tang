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
 * The float boxes of AD-27: the engine's half of the reservation.
 *
 * Compiled code holds a float raw, and a Tang float is a heap object, so a
 * conversion that "cannot fail or collect" needs the object to exist already.
 * The boxes are made here, ahead of time, at the points where an allocation can
 * be answered (entering compiled code, and the push hook), kept as precise roots
 * of the execution, and handed out by the descriptor's `convert`. The reservation
 * (runtime-core's cells) is extended by each frame's `max_converting`, and the
 * pool is kept at least as large as the reservation's capacity whenever compiled
 * code may run, which is the same worst case the core's rebuild checks up front.
 *
 * A retract gives back capacity and keeps the boxes, so a warm loop of calls
 * allocates nothing. A box a rebuild consumed is replaced by the next top-up.
 *
 * The pool lives as long as a run does and no longer: when control goes back to the
 * host (a pause, the end, an unwind) it is dropped, so that nothing observable
 * between runs, a snapshot above all (the heap image requires the destination to
 * report as many precise root slots as the source did), depends on it.
 */

#include <ghoti.io/lang-tang/macros.h>

#include "jit_internal.h"

#include <ghoti.io/cutil/memory.h>
#include <ghoti.io/runtime-heap/heap.h>

#include <string.h>

/** Room for `n` boxes in the pool's array. */
static bool pool_room(GLTANG_Execution * exec, GLTANG_Jit * jit, size_t n) {
  if (n <= jit->pool_capacity) {
    return true;
  }
  size_t capacity = jit->pool_capacity ? jit->pool_capacity : 8u;
  while (capacity < n) {
    capacity *= 2u;
  }
  GLTANG_Value * grown = gcu_allocator_realloc(GLTANG_JIT_ALLOCATOR(exec), jit->pool, capacity * sizeof(GLTANG_Value));
  if (!grown) {
    return false;
  }
  jit->pool = grown;
  jit->pool_capacity = capacity;
  return true;
}

/**
 * Tops the pool up to `want` boxes. Allocates straight from the heap, so a
 * refusal (the memory budget, the system) is an answer to the caller and not a
 * runtime poll: the interpreter would make the poll when it made the allocation
 * itself, and it will, because the entry or the call is then left to it.
 */
static bool pool_top_up(GLTANG_Execution * exec, GLTANG_Jit * jit, size_t want) {
  if (jit->pool_count >= want) {
    return true;
  }
  if (!pool_room(exec, jit, want)) {
    return false;
  }
  while (jit->pool_count < want) {
    void * object = NULL;
    if (grheap_alloc_sized(exec->heap, &gltang_type_float, sizeof(GLTANG_FloatObject), &object) != GRHEAP_OK) {
      return false;
    }
    GLTANG_FloatObject * box = object;
    box->kind = GLTANG_OBJ_FLOAT;
    box->reserved = 0;
    box->value = 0.0;
    // Rooted the moment it is stored; nothing collects between the allocation and here.
    jit->pool[jit->pool_count++] = gltang_value_of(box);
    ++exec->jit_stats.box_pool_made;
  }
  return true;
}

bool gltang_jit_extend(GLTANG_Execution * exec, size_t cells, uint64_t level, uint64_t fword) {
  if (cells == 0) {
    return true;
  }
  GLTANG_Jit * jit = exec->jit;
  const uint64_t nth = ++jit->test_extends;
  if (jit->test_fail_extend_at != 0 && nth == jit->test_fail_extend_at) {
    ++exec->jit_stats.reservation_refusals;
    return false;
  }
  if (jit->ext_count == jit->ext_capacity) {
    size_t capacity = jit->ext_capacity ? jit->ext_capacity * 2u : 16u;
    GLTANG_JitExtension * grown = gcu_allocator_realloc(GLTANG_JIT_ALLOCATOR(exec), jit->ext, capacity * sizeof(GLTANG_JitExtension));
    if (!grown) {
      ++exec->jit_stats.reservation_refusals;
      return false;
    }
    jit->ext = grown;
    jit->ext_capacity = capacity;
  }
  if (grcore_deopt_reservation_extend(exec->context, jit->reservation, cells) != GRCORE_OK) {
    ++exec->jit_stats.reservation_refusals;
    return false;
  }
  const size_t capacity = grcore_deopt_reservation_capacity(jit->reservation);
  if ((jit->test_fail_pool_at != 0 && nth == jit->test_fail_pool_at) || !pool_top_up(exec, jit, capacity)) {
    grcore_deopt_reservation_retract(jit->reservation, cells);
    ++exec->jit_stats.box_pool_refusals;
    return false;
  }
  jit->ext[jit->ext_count].level = level;
  jit->ext[jit->ext_count].fword = fword;
  jit->ext[jit->ext_count].cells = cells;
  ++jit->ext_count;
  if (capacity > exec->jit_stats.reservation_peak) {
    exec->jit_stats.reservation_peak = capacity;
  }
  return true;
}

void gltang_jit_retract_to(GLTANG_Execution * exec, size_t count) {
  GLTANG_Jit * jit = exec->jit;
  while (jit->ext_count > count) {
    grcore_deopt_reservation_retract(jit->reservation, jit->ext[--jit->ext_count].cells);
  }
}

void gltang_jit_retract_level(GLTANG_Execution * exec, uint64_t level, uint64_t fword) {
  GLTANG_Jit * jit = exec->jit;
  if (jit->ext_count > jit->ext_floor && jit->ext[jit->ext_count - 1u].level == level && jit->ext[jit->ext_count - 1u].fword == fword) {
    grcore_deopt_reservation_retract(jit->reservation, jit->ext[--jit->ext_count].cells);
  }
}

void gltang_jit_release_pool(GLTANG_Execution * exec) {
  GLTANG_Jit * jit = exec->jit;
  if (!jit) {
    return;
  }
  // With control back at the host no compiled frame is open: a pause rebuilt every
  // one of them, an unwind skipped them, and every exit from compiled code gave back
  // the entry's extension and its calls' (`gltang_jit_enter`), so the reservation is
  // at its base, which is zero for the outermost run. A pool dropped while a
  // reservation was still extended would break the invariant that the pool is as large
  // as the capacity.
  assert(grcore_deopt_reservation_capacity(jit->reservation) == 0 && jit->ext_count == 0);
  for (size_t i = 0; i < jit->pool_count; ++i) {
    jit->pool[i] = 0;
  }
  jit->pool_count = 0;
}

void gltang_jit_enumerate_roots(GLTANG_Execution * exec, const GRCORE_RootVisitor * visitor) {
  GLTANG_Jit * jit = exec->jit;
  if (!jit || !visitor->slot) {
    return;
  }
  for (size_t i = 0; i < jit->pool_count; ++i) {
    visitor->slot(visitor->user, &jit->pool[i]);
  }
}

void gltang_jit_convert(GRCORE_Context * context, GRCORE_Representation representation, uint64_t raw, uint64_t * out_root) {
  GLTANG_Execution * exec = gltang_vm_execution_of(context);
  GLTANG_Jit * jit = exec ? exec->jit : NULL;
  if (representation != GRCORE_REPR_F64 || !jit || jit->pool_count == 0) {
    // Never emitted, and never a correct run: the pool is as large as the
    // reservation, which the rebuild checked. Null is a value the collector reads.
    if (exec) {
      ++exec->jit_stats.conversion_errors;
    }
    *out_root = 0;
    return;
  }
  GLTANG_Value v = jit->pool[--jit->pool_count];
  jit->pool[jit->pool_count] = 0;
  GLTANG_FloatObject * box = gltang_object(v);
  memcpy(&box->value, &raw, sizeof(box->value));
  ++exec->jit_stats.values_boxed;
  *out_root = v;
}

bool gltang_jit_reverse(GRCORE_Context * context, GRCORE_Representation representation, uint64_t value, uint64_t * out_raw) {
  if (representation != GRCORE_REPR_F64) {
    GLTANG_Execution * exec = gltang_vm_execution_of(context);
    if (exec) {
      ++exec->jit_stats.conversion_errors;
    }
    return false;
  }
  if (!gltang_v_is_kind(value, GLTANG_OBJ_FLOAT)) {
    return false;
  }
  memcpy(out_raw, &((const GLTANG_FloatObject *)gltang_object(value))->value, sizeof(*out_raw));
  return true;
}
