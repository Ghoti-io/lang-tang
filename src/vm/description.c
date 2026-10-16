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
 * Building a description (description.h): the nodes, the limits, the one-parent
 * rule, the UTF-8 check, and the reference count.
 *
 * Every refusal happens before anything is written: a call that cannot finish
 * leaves the description as it found it. The steps that can fail for want of
 * memory (growing the node array and the byte store) come after every check and
 * before the first write, and the writes that follow cannot fail.
 */

#include <ghoti.io/lang-tang/macros.h>

#include <string.h>
#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/unicode/utf.h>
#include "description_internal.h"
#include "test_hooks.h"

#define INITIAL_NODES 16u

/** The next owner number: each description's refs carry its own, so a ref cannot be used with another description. */
static _Atomic uint32_t g_next_owner = 1;

static uint32_t next_owner(void) {
  uint32_t owner = atomic_fetch_add_explicit(&g_next_owner, 1, memory_order_relaxed);
  return owner ? owner : atomic_fetch_add_explicit(&g_next_owner, 1, memory_order_relaxed);
}

static GLTANG_Result create(const GLTANG_Allocator * allocator, bool in_run, const GLTANG_ValueLimits * limits, GLTANG_ValueDescription ** out) {
  if (!out) {
    return GLTANG_ERR_INVALID;
  }
  size_t max_nodes = GLTANG_VALUE_DEFAULT_NODES;
  size_t max_bytes = GLTANG_VALUE_DEFAULT_BYTES;
  size_t max_depth = GLTANG_VALUE_DEFAULT_DEPTH;
  if (limits) {
    if (limits->size != sizeof(GLTANG_ValueLimits)) {
      return GLTANG_ERR_INVALID;
    }
    if (limits->max_nodes > GLTANG_VALUE_MAX_NODES || limits->max_bytes > GLTANG_VALUE_MAX_BYTES || limits->max_depth > GLTANG_VALUE_MAX_DEPTH) {
      return GLTANG_ERR_LIMIT;
    }
    max_nodes = limits->max_nodes ? limits->max_nodes : max_nodes;
    max_bytes = limits->max_bytes ? limits->max_bytes : max_bytes;
    max_depth = limits->max_depth ? limits->max_depth : max_depth;
  }
  GLTANG_ValueDescription * d = gcu_allocator_calloc(allocator, 1, sizeof(*d));
  if (!d) {
    return GLTANG_ERR_OOM;
  }
  d->nodes = gcu_allocator_calloc(allocator, INITIAL_NODES, sizeof(GLTANG_DescNode));
  if (!d->nodes) {
    gcu_allocator_free(allocator, d);
    return GLTANG_ERR_OOM;
  }
  atomic_init(&d->references, 1);
  atomic_init(&d->sealed, false);
  d->allocator = allocator;
  d->in_run = in_run;
  d->owner = next_owner();
  d->max_nodes = max_nodes;
  d->max_bytes = max_bytes;
  d->max_depth = max_depth;
  d->node_used = 1;
  d->node_capacity = INITIAL_NODES;
  *out = d;
  return GLTANG_OK;
}

static void destroy(GLTANG_ValueDescription * d) {
  const GLTANG_Allocator * allocator = d->allocator;
  gcu_allocator_free(allocator, d->nodes);
  gcu_allocator_free(allocator, d->bytes);
  gcu_allocator_free(allocator, d);
}

GLTANG_Result gltang_value_description_create(const GLTANG_ValueLimits * limits, GLTANG_ValueDescription ** out_description) {
  return create(NULL, false, limits, out_description);
}

GLTANG_Result gltang_vm_description_create_in_run(const GLTANG_Allocator * allocator, GLTANG_ValueDescription ** out) {
  return create(allocator, true, NULL, out);
}

void gltang_vm_description_destroy_in_run(GLTANG_ValueDescription * description) {
  if (description) {
    destroy(description);
  }
}

GLTANG_ValueDescription * gltang_value_description_retain(GLTANG_ValueDescription * description) {
  if (!description || description->in_run) {
    return NULL;
  }
  atomic_fetch_add_explicit(&description->references, 1, memory_order_relaxed);
  return description;
}

void gltang_value_description_release(GLTANG_ValueDescription * description) {
  if (!description || description->in_run) {
    return;
  }
  // The last reference frees. acq_rel so that every other thread's reads of the
  // description happen before the free.
  if (atomic_fetch_sub_explicit(&description->references, 1, memory_order_acq_rel) == 1) {
    destroy(description);
  }
}

GLTANG_Result gltang_value_description_seal(GLTANG_ValueDescription * description) {
  if (!description) {
    return GLTANG_ERR_INVALID;
  }
  atomic_store_explicit(&description->sealed, true, memory_order_release);
  return GLTANG_OK;
}

bool gltang_value_description_sealed(const GLTANG_ValueDescription * description) {
  return description && atomic_load_explicit(&((GLTANG_ValueDescription *)(uintptr_t)description)->sealed, memory_order_acquire);
}

size_t gltang_value_description_nodes(const GLTANG_ValueDescription * description) {
  return description ? gltang_desc_node_count(description) : 0;
}

size_t gltang_value_description_bytes(const GLTANG_ValueDescription * description) {
  return description ? description->counted_bytes : 0;
}

// ---------------------------------------------------------------------------
// Making nodes
// ---------------------------------------------------------------------------

/** Whether `extra` more bytes, and one more node when `node`, fit under the limits. */
static GLTANG_Result room_for(const GLTANG_ValueDescription * d, size_t extra, bool node) {
  if (node && gltang_desc_node_count(d) >= d->max_nodes) {
    return GLTANG_ERR_LIMIT;
  }
  size_t add = extra + (node ? GLTANG_NODE_COST : 0u);
  if (extra > d->max_bytes || d->counted_bytes > d->max_bytes || add > d->max_bytes - d->counted_bytes) {
    return GLTANG_ERR_LIMIT;
  }
  return GLTANG_OK;
}

/** Makes sure the node array has room for one more and the byte store for `extra` more. */
static GLTANG_Result reserve(GLTANG_ValueDescription * d, size_t extra, bool node) {
  if (node && d->node_used == d->node_capacity) {
    size_t capacity = d->node_capacity * 2u;
    GLTANG_DescNode * grown = gcu_allocator_realloc(d->allocator, d->nodes, capacity * sizeof(GLTANG_DescNode));
    if (!grown) {
      return GLTANG_ERR_OOM;
    }
    d->nodes = grown;
    d->node_capacity = capacity;
  }
  if (extra > d->byte_capacity - d->byte_used) {
    size_t capacity = d->byte_capacity ? d->byte_capacity : 256u;
    while (capacity - d->byte_used < extra) {
      capacity *= 2u;
    }
    char * grown = gcu_allocator_realloc(d->allocator, d->bytes, capacity);
    if (!grown) {
      return GLTANG_ERR_OOM;
    }
    d->bytes = grown;
    d->byte_capacity = capacity;
  }
  return GLTANG_OK;
}

/** Copies `length` bytes into the byte store; the room is already there. */
static uint32_t store_bytes(GLTANG_ValueDescription * d, const char * text, size_t length) {
  uint32_t offset = (uint32_t)d->byte_used;
  if (length) {
    memcpy(d->bytes + d->byte_used, text, length);
  }
  d->byte_used += length;
  d->counted_bytes += length;
  return offset;
}

static bool open_for_building(const GLTANG_ValueDescription * d) {
  return d && !atomic_load_explicit(&((GLTANG_ValueDescription *)(uintptr_t)d)->sealed, memory_order_acquire);
}

/** The text's validity, once: UTF-8 and a segment tag in range. */
static GLTANG_Result check_text(const char * text, size_t length, GLTANG_String_Type encoding) {
  if ((!text && length) || (unsigned)encoding > (unsigned)GLTANG_UNICODE_STRING_TYPE_JAVASCRIPT) {
    return GLTANG_ERR_INVALID;
  }
  if (length && guni_utf8_validate(text, length, NULL) != GUNI_OK) {
    return GLTANG_ERR_INVALID;
  }
  return GLTANG_OK;
}

/**
 * Makes a node of `kind` with `extra` bytes of text, all checks first and every
 * write after the last step that can fail.
 */
static GLTANG_Result make(GLTANG_ValueDescription * d, GLTANG_NodeKind kind, const char * text, size_t extra, GLTANG_String_Type tag, GLTANG_DescNode ** out_node, GLTANG_ValueRef * out_ref) {
  if (!open_for_building(d) || !out_ref) {
    return GLTANG_ERR_INVALID;
  }
  GLTANG_Result r = room_for(d, extra, true);
  if (r != GLTANG_OK) {
    return r;
  }
  r = reserve(d, extra, true);
  if (r != GLTANG_OK) {
    return r;
  }
  uint32_t index = (uint32_t)d->node_used++;
  GLTANG_DescNode * node = &d->nodes[index];
  memset(node, 0, sizeof(*node));
  node->kind = (uint8_t)kind;
  d->counted_bytes += GLTANG_NODE_COST;
  if (kind == GLTANG_NODE_STRING) {
    node->tag = (uint8_t)tag;
    node->as.text.offset = store_bytes(d, text, extra);
    node->as.text.length = (uint32_t)extra;
  }
  out_ref->index = index;
  out_ref->owner = d->owner;
  *out_node = node;
  return GLTANG_OK;
}

GLTANG_Result gltang_value_null(GLTANG_ValueDescription * description, GLTANG_ValueRef * out_ref) {
  GLTANG_DescNode * node;
  return make(description, GLTANG_NODE_NULL, NULL, 0, GLTANG_UNICODE_STRING_TYPE_TRUSTED, &node, out_ref);
}

GLTANG_Result gltang_value_bool(GLTANG_ValueDescription * description, bool value, GLTANG_ValueRef * out_ref) {
  GLTANG_DescNode * node;
  GLTANG_Result r = make(description, GLTANG_NODE_BOOL, NULL, 0, GLTANG_UNICODE_STRING_TYPE_TRUSTED, &node, out_ref);
  if (r == GLTANG_OK) {
    node->as.integer = value ? 1 : 0;
  }
  return r;
}

GLTANG_Result gltang_value_integer(GLTANG_ValueDescription * description, int64_t value, GLTANG_ValueRef * out_ref) {
  GLTANG_DescNode * node;
  GLTANG_Result r = make(description, GLTANG_NODE_INTEGER, NULL, 0, GLTANG_UNICODE_STRING_TYPE_TRUSTED, &node, out_ref);
  if (r == GLTANG_OK) {
    node->as.integer = value;
  }
  return r;
}

GLTANG_Result gltang_value_float(GLTANG_ValueDescription * description, double value, GLTANG_ValueRef * out_ref) {
  GLTANG_DescNode * node;
  GLTANG_Result r = make(description, GLTANG_NODE_FLOAT, NULL, 0, GLTANG_UNICODE_STRING_TYPE_TRUSTED, &node, out_ref);
  if (r == GLTANG_OK) {
    node->as.number = value;
  }
  return r;
}

GLTANG_Result gltang_value_string(GLTANG_ValueDescription * description, const char * text, size_t length, GLTANG_String_Type encoding, GLTANG_ValueRef * out_ref) {
  if (!description || !out_ref) {
    return GLTANG_ERR_INVALID;
  }
  // The limit first: the text is read for as many bytes as the caller says, and
  // a count past the limit is refused before any of them is looked at.
  if (length > description->max_bytes) {
    return GLTANG_ERR_LIMIT;
  }
  GLTANG_Result r = check_text(text, length, encoding);
  if (r != GLTANG_OK) {
    return r;
  }
  GLTANG_DescNode * node;
  return make(description, GLTANG_NODE_STRING, text, length, encoding, &node, out_ref);
}

GLTANG_Result gltang_value_array(GLTANG_ValueDescription * description, GLTANG_ValueRef * out_ref) {
  GLTANG_DescNode * node;
  GLTANG_Result r = make(description, GLTANG_NODE_ARRAY, NULL, 0, GLTANG_UNICODE_STRING_TYPE_TRUSTED, &node, out_ref);
  if (r == GLTANG_OK) {
    node->extent = 1;
  }
  return r;
}

GLTANG_Result gltang_value_map(GLTANG_ValueDescription * description, GLTANG_ValueRef * out_ref) {
  GLTANG_DescNode * node;
  GLTANG_Result r = make(description, GLTANG_NODE_MAP, NULL, 0, GLTANG_UNICODE_STRING_TYPE_TRUSTED, &node, out_ref);
  if (r == GLTANG_OK) {
    node->extent = 1;
  }
  return r;
}

// ---------------------------------------------------------------------------
// Attaching
// ---------------------------------------------------------------------------

static bool valid_ref(const GLTANG_ValueDescription * d, GLTANG_ValueRef ref) {
  return ref.index > 0 && ref.index < d->node_used && ref.owner == d->owner;
}

/**
 * Attaches `child` under `container` (an array, or a map with a key). Every
 * refusal is before the first write.
 */
static GLTANG_Result attach(GLTANG_ValueDescription * d, GLTANG_ValueRef container, GLTANG_ValueRef child, GLTANG_NodeKind want, const char * key, size_t key_length, GLTANG_String_Type key_encoding) {
  if (!open_for_building(d) || !valid_ref(d, container) || !valid_ref(d, child)) {
    return GLTANG_ERR_INVALID;
  }
  GLTANG_DescNode * parent = &d->nodes[container.index];
  GLTANG_DescNode * node = &d->nodes[child.index];
  if (parent->kind != (uint8_t)want) {
    return GLTANG_ERR_INVALID;
  }
  if (want == GLTANG_NODE_MAP) {
    if (key_length > d->max_bytes) {
      return GLTANG_ERR_LIMIT;
    }
    GLTANG_Result r = check_text(key, key_length, key_encoding);
    if (r != GLTANG_OK) {
      return r;
    }
  }
  // One parent only: a node already under a container, or the root, is not
  // attached again.
  if (node->parent != 0) {
    return GLTANG_ERR_INVALID;
  }
  // The ancestors of the container, from its own parent upward: the child must
  // not be one of them (that is a cycle), and their number is the container's
  // level. The walk is as long as the nesting the limit allows and no longer.
  size_t level = 0;
  size_t bound = d->max_depth + 2u;
  if (child.index == container.index) {
    return GLTANG_ERR_INVALID;
  }
  for (uint32_t at = container.index; d->nodes[at].parent != 0 && d->nodes[at].parent != GLTANG_NODE_ROOT; at = d->nodes[at].parent) {
    if (d->nodes[at].parent == child.index) {
      return GLTANG_ERR_INVALID;
    }
    if (++level > bound) {
      return GLTANG_ERR_LIMIT;
    }
  }
  // The container sits at `level`, the child one below it, and the child's own
  // subtree reaches `extent` further.
  if (level + 1u + node->extent > d->max_depth) {
    return GLTANG_ERR_LIMIT;
  }
  GLTANG_Result r = room_for(d, want == GLTANG_NODE_MAP ? key_length : 0u, false);
  if (r != GLTANG_OK) {
    return r;
  }
  r = reserve(d, want == GLTANG_NODE_MAP ? key_length : 0u, false);
  if (r != GLTANG_OK) {
    return r;
  }
  // Nothing below can fail.
  node->parent = container.index;
  if (want == GLTANG_NODE_MAP) {
    node->key_tag = (uint8_t)key_encoding;
    node->key_offset = store_bytes(d, key, key_length);
    node->key_length = (uint32_t)key_length;
  }
  if (parent->last) {
    d->nodes[parent->last].next = child.index;
  }
  else {
    parent->first = child.index;
  }
  parent->last = child.index;
  ++parent->count;
  // Every ancestor now reaches at least as deep as the child's subtree does.
  uint32_t reach = node->extent + 1u;
  for (uint32_t at = container.index; at != 0 && at != GLTANG_NODE_ROOT && d->nodes[at].extent < reach; at = d->nodes[at].parent) {
    d->nodes[at].extent = reach++;
  }
  return GLTANG_OK;
}

GLTANG_Result gltang_value_array_push(GLTANG_ValueDescription * description, GLTANG_ValueRef array, GLTANG_ValueRef element) {
  if (!description) {
    return GLTANG_ERR_INVALID;
  }
  return attach(description, array, element, GLTANG_NODE_ARRAY, NULL, 0, GLTANG_UNICODE_STRING_TYPE_TRUSTED);
}

GLTANG_Result gltang_value_map_set(GLTANG_ValueDescription * description, GLTANG_ValueRef map, const char * key, size_t key_length, GLTANG_String_Type key_encoding, GLTANG_ValueRef value) {
  if (!description) {
    return GLTANG_ERR_INVALID;
  }
  return attach(description, map, value, GLTANG_NODE_MAP, key, key_length, key_encoding);
}

GLTANG_Result gltang_value_set_root(GLTANG_ValueDescription * description, GLTANG_ValueRef value) {
  if (!open_for_building(description) || !valid_ref(description, value)) {
    return GLTANG_ERR_INVALID;
  }
  GLTANG_DescNode * node = &description->nodes[value.index];
  if (node->parent != 0) {
    return GLTANG_ERR_INVALID;
  }
  if (description->root) {
    // The earlier root is a free node again.
    description->nodes[description->root].parent = 0;
  }
  node->parent = GLTANG_NODE_ROOT;
  description->root = value.index;
  return GLTANG_OK;
}

// ---------------------------------------------------------------------------
// The tests' hook
// ---------------------------------------------------------------------------

/*
 * For the tests only: sets the limits as they are, with no ceiling, so that a
 * test can build a description the engine's own limits would refuse and show
 * that the materializer refuses it again. Declared in vm/test_hooks.h.
 */
void gltang_vm_test_description_limits(GLTANG_ValueDescription * description, size_t max_nodes, size_t max_bytes, size_t max_depth) {
  description->max_nodes = max_nodes;
  description->max_bytes = max_bytes;
  description->max_depth = max_depth;
}
