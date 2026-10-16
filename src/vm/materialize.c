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
 * Making a value from a description (description.h): the copy of a host's tree
 * into the heap of the execution that reached it.
 *
 * It is `copy_rec` (container.c) with a different source, and the same rules:
 * every partial value is in a temporary root, every pointer store goes through
 * the collector's barrier (the array and map calls make them), and nothing
 * collects between a child's return and its store. A container is made at its
 * exact size and held in a temporary while its children are made; a map entry's
 * child is held while its key is made, and both are stored by a call that
 * allocates nothing.
 *
 * Work is charged before it is done, one node at a time (8 bytes for the node,
 * and the bytes of its string and key), through a pacer that polls every
 * GLTANG_POLL_BYTES: a copy of a large description runs the budget out and
 * unwinds like a long string copy, and the remainder that never reached a poll
 * is added to the fuel the run has pending.
 *
 * The description's own limits were checked when it was built, and are checked
 * again here (the engine does not trust the description to have kept them), as
 * is the depth: the recursion stops at the engine's ceiling whatever the
 * description says of its own depth.
 *
 * A description holds no pointer into any heap and is only read, so any number
 * of executions on any number of threads may materialize one sealed description
 * at the same time.
 */

#include <ghoti.io/lang-tang/macros.h>

#include <stdatomic.h>
#include "description_internal.h"
#include "test_hooks.h"
#include "vm_internal.h"

enum { MAT_OK = 0, MAT_OOM, MAT_UNWIND, MAT_DEEP };

/*
 * The ceilings no description may be raised past (description.h). They are
 * variables only so that a test can lower them (gltang_vm_test_materialize_ceilings,
 * declared in vm/test_hooks.h) and show that the materializer holds a
 * description to them whatever it says of its own limits; nothing else writes them.
 */
static _Atomic size_t g_node_ceiling = GLTANG_VALUE_MAX_NODES;
static _Atomic size_t g_byte_ceiling = GLTANG_VALUE_MAX_BYTES;

typedef struct Materializer {
  GLTANG_Execution * exec;
  const GLTANG_ValueDescription * description;
  size_t since;   ///< Bytes of work charged since the last poll.
} Materializer;

/** Charges `bytes` of work; polls when enough has gathered. Returns whether the run goes on. */
static bool charge(Materializer * m, size_t bytes) {
  m->since += bytes;
  if (m->since >= GLTANG_POLL_BYTES) {
    size_t work = m->since / GLTANG_WORK_BYTES_PER_FUEL;
    m->since = 0;
    return GLTANG_NATIVE_POLL(m->exec, DESCRIPTION_MATERIALIZE, work ? work : 1) == GLTANG_ST_OK;
  }
  return true;
}

/** Whether `v` is what an allocation answers when it fails; says which way in `fail`. */
static bool failed(const Materializer * m, GLTANG_Value v, int * fail) {
  if (v == GLTANG_V_UNWIND) {
    *fail = MAT_UNWIND;
    return true;
  }
  if (v == m->exec->roots[GLTANG_ROOT_OOM]) {
    *fail = MAT_OOM;
    return true;
  }
  return false;
}

static GLTANG_Value make_rec(Materializer * m, uint32_t index, int depth, int * fail) {
  GLTANG_Execution * exec = m->exec;
  const GLTANG_ValueDescription * d = m->description;
  const GLTANG_DescNode * node = &d->nodes[index];
  GLTANG_Value v;
  switch ((GLTANG_NodeKind)node->kind) {
    case GLTANG_NODE_NULL:
    case GLTANG_NODE_BOOL:
      // Nothing is allocated, but the node is still work: an array of millions
      // of them is a loop the budget must be able to stop.
      if (!charge(m, 8u)) {
        *fail = MAT_UNWIND;
        return GLTANG_V_NULL;
      }
      return node->kind == GLTANG_NODE_NULL ? GLTANG_V_NULL : gltang_v_from_bool(node->as.integer != 0);
    case GLTANG_NODE_INTEGER:
      if (!charge(m, 8u)) {
        *fail = MAT_UNWIND;
        return GLTANG_V_NULL;
      }
      v = gltang_vm_make_int(exec, node->as.integer);
      return failed(m, v, fail) ? GLTANG_V_NULL : v;
    case GLTANG_NODE_FLOAT:
      if (!charge(m, 8u)) {
        *fail = MAT_UNWIND;
        return GLTANG_V_NULL;
      }
      v = gltang_vm_make_float(exec, node->as.number);
      return failed(m, v, fail) ? GLTANG_V_NULL : v;
    case GLTANG_NODE_STRING:
      if (!charge(m, 8u + node->as.text.length)) {
        *fail = MAT_UNWIND;
        return GLTANG_V_NULL;
      }
      v = gltang_vm_string_from_trusted_utf8(exec, gltang_desc_bytes(d, node->as.text.offset), node->as.text.length, (GLTANG_String_Type)node->tag);
      if (failed(m, v, fail)) {
        return GLTANG_V_NULL;
      }
      if (v == GLTANG_V_NULL) {
        // Text that was checked when it was added and still has no graphemes:
        // not a state a description can be in.
        *fail = MAT_OOM;
      }
      return v;
    case GLTANG_NODE_ARRAY:
    case GLTANG_NODE_MAP:
      break;
    default:
      return GLTANG_V_NULL;
  }
  if (depth >= GLTANG_MAX_VALUE_DEPTH) {
    *fail = MAT_DEEP;
    return GLTANG_V_NULL;
  }
  const bool is_array = node->kind == GLTANG_NODE_ARRAY;
  const size_t n = node->count;
  if (!charge(m, 16u)) {
    *fail = MAT_UNWIND;
    return GLTANG_V_NULL;
  }
  size_t mark = gltang_vm_temp_mark(exec);
  GLTANG_Value copy = is_array ? gltang_vm_array_new(exec, n) : gltang_vm_map_new(exec, n);
  if (!gltang_v_is_kind(copy, is_array ? GLTANG_OBJ_ARRAY : GLTANG_OBJ_MAP)) {
    *fail = copy == GLTANG_V_UNWIND ? MAT_UNWIND : MAT_OOM;
    return GLTANG_V_NULL;
  }
  // Every call below can collect, and the copy is in no root of its own: hold
  // it, and read it again from the temporary each time it is wanted.
  if (!gltang_vm_temp_push(exec, copy)) {
    *fail = MAT_OOM;
    return GLTANG_V_NULL;
  }
  for (uint32_t at = node->first; at && !*fail; at = d->nodes[at].next) {
    const GLTANG_DescNode * child = &d->nodes[at];
    GLTANG_Value value = make_rec(m, at, depth + 1, fail);
    if (*fail) {
      break;
    }
    if (is_array) {
      // Nothing collects between the child's return and its being stored.
      gltang_vm_array_push(exec, gltang_vm_temp_at(exec, mark), value);
      continue;
    }
    // The entry's value is held while its key is made.
    size_t held = gltang_vm_temp_mark(exec);
    if (!gltang_vm_temp_push(exec, value)) {
      *fail = MAT_OOM;
      break;
    }
    if (!charge(m, 8u + child->key_length)) {
      *fail = MAT_UNWIND;
      break;
    }
    GLTANG_Value key = gltang_vm_string_from_trusted_utf8(exec, gltang_desc_bytes(d, child->key_offset), child->key_length, (GLTANG_String_Type)child->key_tag);
    if (failed(m, key, fail)) {
      break;
    }
    if (key == GLTANG_V_NULL) {
      *fail = MAT_OOM;
      break;
    }
    // A key set twice keeps its first position and takes the last value: the
    // map's own fill says so.
    GLTANG_Value pair[2] = {key, gltang_vm_temp_at(exec, held)};
    gltang_vm_map_fill(exec, gltang_vm_temp_at(exec, mark), pair, 1);
    gltang_vm_temp_release(exec, held);
  }
  copy = gltang_vm_temp_at(exec, mark);
  gltang_vm_temp_release(exec, mark);
  return copy;
}

GLTANG_Value gltang_vm_materialize(GLTANG_Execution * exec, const GLTANG_ValueDescription * description) {
  if (!description || !description->root) {
    return GLTANG_V_NULL;
  }
  // The limits the description was built under, once more: a description that
  // has somehow passed them is refused as a request too large for the heap.
  const size_t node_ceiling = atomic_load_explicit(&g_node_ceiling, memory_order_relaxed);
  const size_t byte_ceiling = atomic_load_explicit(&g_byte_ceiling, memory_order_relaxed);
  size_t node_limit = description->max_nodes < node_ceiling ? description->max_nodes : node_ceiling;
  size_t byte_limit = description->max_bytes < byte_ceiling ? description->max_bytes : byte_ceiling;
  if (gltang_desc_node_count(description) > node_limit) {
    return exec->roots[GLTANG_ROOT_OOM];
  }
  if (description->counted_bytes > byte_limit) {
    return exec->roots[GLTANG_ROOT_OOM];
  }
  Materializer m = {exec, description, 0};
  int fail = MAT_OK;
  GLTANG_Value v = make_rec(&m, description->root, 0, &fail);
  // What never reached a poll is still work.
  exec->pending_fuel += m.since / GLTANG_WORK_BYTES_PER_FUEL;
  switch (fail) {
    case MAT_OK: return v;
    case MAT_UNWIND: return GLTANG_V_UNWIND;
    case MAT_DEEP: return gltang_vm_make_error(exec, GLTANG_ERROR_RECURSION_LIMIT);
    default: return exec->roots[GLTANG_ROOT_OOM];
  }
}

/*
 * For the tests only: lowers the ceilings the materializer holds a description to
 * (the documented ones are the defaults; pass them to put them back). Declared in
 * vm/test_hooks.h.
 */
void gltang_vm_test_materialize_ceilings(size_t max_nodes, size_t max_bytes) {
  atomic_store_explicit(&g_node_ceiling, max_nodes, memory_order_relaxed);
  atomic_store_explicit(&g_byte_ceiling, max_bytes, memory_order_relaxed);
}
