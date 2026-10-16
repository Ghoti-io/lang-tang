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
 * The inside of a description: its nodes, and what the materializer reads from
 * them (description.h says what a host sees).
 *
 * A description is plain memory from the allocator it was made with (the
 * process's for a host-built one, the context's for one built during a call), and
 * holds no pointer into any heap. Its nodes are in one array and name each other
 * by index, so the array can grow while a host builds. Index 0 is no node. Once
 * sealed it is never written, which is what lets any number of threads read it.
 */

#ifndef GHOTI_IO_GLTANG_VM_DESCRIPTION_INTERNAL_H
#define GHOTI_IO_GLTANG_VM_DESCRIPTION_INTERNAL_H

#include <ghoti.io/lang-tang/macros.h>

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <ghoti.io/lang-tang/description.h>

/**
 * @brief What a node is. The numbering is open: later stories add the node
 *   kinds of bound natives, libraries and host-typed instances after MAP.
 */
typedef enum {
  GLTANG_NODE_NONE = 0,
  GLTANG_NODE_NULL,
  GLTANG_NODE_BOOL,
  GLTANG_NODE_INTEGER,
  GLTANG_NODE_FLOAT,
  GLTANG_NODE_STRING,
  GLTANG_NODE_ARRAY,
  GLTANG_NODE_MAP
} GLTANG_NodeKind;

/** @brief What a node counts against ::GLTANG_ValueLimits::max_bytes besides its bytes. */
#define GLTANG_NODE_COST 16u

/** @brief A node's parent when it is the description's root. */
#define GLTANG_NODE_ROOT UINT32_MAX

/** @brief One node. A child names its siblings; a map's child also holds its key. */
typedef struct GLTANG_DescNode {
  uint8_t kind;               ///< A GLTANG_NodeKind.
  uint8_t tag;                ///< STRING: the segment tag.
  uint8_t key_tag;            ///< A map entry: the key's tag.
  uint8_t reserved;
  uint32_t parent;            ///< 0: none yet; GLTANG_NODE_ROOT: the root; else the container.
  uint32_t extent;            ///< 0 for a scalar; for a container, 1 more than the deepest child's.
  uint32_t first;             ///< A container: its first child, or 0.
  uint32_t last;              ///< A container: its last child, or 0.
  uint32_t next;              ///< The next sibling, or 0.
  uint32_t count;             ///< A container: how many children.
  uint32_t key_offset;        ///< A map entry: the key's bytes, in the description's byte store.
  uint32_t key_length;
  union {
    int64_t integer;
    double number;
    struct { uint32_t offset; uint32_t length; } text;   ///< STRING: in the byte store.
  } as;
} GLTANG_DescNode;

struct GLTANG_ValueDescription {
  atomic_size_t references;
  atomic_bool sealed;
  const GLTANG_Allocator * allocator;   ///< NULL: the default.
  bool in_run;                          ///< Built during a call: the call frees it.
  uint32_t owner;                       ///< What its refs carry (never 0), so a ref of another description is told.
  size_t max_nodes;
  size_t max_bytes;
  size_t max_depth;
  GLTANG_DescNode * nodes;              ///< nodes[0] is no node.
  size_t node_used;                     ///< Entries in use, the unused 0 included.
  size_t node_capacity;
  char * bytes;                         ///< Every string's and key's bytes.
  size_t byte_used;
  size_t byte_capacity;
  size_t counted_bytes;                 ///< What the limit counts: the bytes and GLTANG_NODE_COST a node.
  uint32_t root;                        ///< 0: none, so `null`.
};

/** @brief The nodes a description holds (the unused 0 is not one). */
static inline size_t gltang_desc_node_count(const GLTANG_ValueDescription * d) {
  return d->node_used - 1u;
}

/** @brief The bytes of the string or key at `offset`. */
static inline const char * gltang_desc_bytes(const GLTANG_ValueDescription * d, uint32_t offset) {
  return d->bytes ? d->bytes + offset : "";
}

/**
 * @brief Makes a description whose memory comes from `allocator` and that the
 *   caller owns (a native's, built during its call). It cannot be retained.
 */
GLTANG_Result gltang_vm_description_create_in_run(const GLTANG_Allocator * allocator, GLTANG_ValueDescription ** out);

/** @brief Frees a description made by ::gltang_vm_description_create_in_run. */
void gltang_vm_description_destroy_in_run(GLTANG_ValueDescription * description);

#endif /* GHOTI_IO_GLTANG_VM_DESCRIPTION_INTERNAL_H */
