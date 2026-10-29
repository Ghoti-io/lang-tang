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
 * The inside of a type registry (type.h): what a type and a registry hold.
 *
 * A registry is built on one thread and sealed when it is first attached;
 * after that nothing in it is written except its reference count and, once,
 * the context a registry of thread-unsafe types is bound to. So the engine
 * reads a type through an instance's pointer with no lock.
 */

#ifndef GHOTI_IO_GLTANG_LIBRARY_TYPES_INTERNAL_H
#define GHOTI_IO_GLTANG_LIBRARY_TYPES_INTERNAL_H

#include <ghoti.io/lang-tang/macros.h>

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <ghoti.io/lang-tang/type.h>

/** @brief A type: the registry's copy of a descriptor, and its identity. */
struct GLTANG_Type {
  const GLTANG_TypeRegistry * registry;  ///< Not retained: the registry owns the type.
  uint64_t registry_id;                  ///< The registry's, copied so that identity needs no second load.
  char name[GLTANG_TYPE_NAME_MAX + 1u];
  size_t name_length;
  uint64_t table_hash;
  uint32_t operations;                   ///< Bit GLTANG_TYPE_OP_* for each operation implemented.
  uint32_t resumable;                    ///< Bit GLTANG_TYPE_OP_* for each operation that may call guest code (descriptor `resumable_ops`, checked).
  GLTANG_Continuation * continuations;   ///< The copy of the descriptor's table (one block: entries, then names); NULL when it declares none. Owned.
  size_t continuation_count;
  GLTANG_TypeDescriptor descriptor;      ///< The copy, `size` the engine's; `name` points at `name` above.
};

/** @brief An attribute a registry adds to a built-in kind (type.h): get only. */
typedef struct GLTANG_HostAttribute {
  GLTANG_ValueKind kind;
  size_t name_length;
  char name[GLTANG_TYPE_NAME_MAX + 1u];
  GLTANG_HostAttributeFn fn;
  void * user;
} GLTANG_HostAttribute;

struct GLTANG_TypeRegistry {
  atomic_size_t references;
  atomic_bool sealed;
  uint64_t id;                           ///< Unique in the process, never reused, never 0.
  bool bindable;                         ///< Holds a type that is not thread_safe: bound to one context.
  atomic_uint_fast64_t bound;            ///< The serial of the context it is bound to, or 0.
  GLTANG_Type ** types;                  ///< Each its own block, so a type's address never changes.
  size_t count;
  size_t capacity;
  GLTANG_HostAttribute * attributes;     ///< Written while the registry is unsealed; read with no lock after.
  size_t attribute_count;
  size_t attribute_capacity;
};

/** @brief The attribute of `kind` and `name` the registry holds, or NULL. */
const GLTANG_HostAttribute * gltang_type_registry_find_attribute(
    const GLTANG_TypeRegistry * registry, GLTANG_ValueKind kind, const char * name, size_t length);

/** @brief Seals a registry and takes a reference (an execution attaches it). */
GLTANG_TypeRegistry * gltang_type_registry_attach(GLTANG_TypeRegistry * registry);

/**
 * @brief Binds a registry of thread-unsafe types to the context whose serial
 *   is `context_serial`, or checks it is bound to it already.
 *
 * A context is named by a serial the engine gives it (never 0, never reused in
 * the process), not by its address, which a later context can be given.
 *
 * @return Whether the registry may be attached to an execution of that
 *   context: true for a registry whose types are all thread-safe, or one bound
 *   to it now or before.
 */
bool gltang_type_registry_bind(GLTANG_TypeRegistry * registry, uint64_t context_serial);

/** @brief Whether an implemented operation of a type may ask for a guest call (type.h). */
static inline bool gltang_type_resumable(const GLTANG_Type * type, GLTANG_TypeOp op) {
  return (type->resumable >> (unsigned)op) & 1u;
}

/** @brief Whether a type implements an operation. */
static inline bool gltang_type_has(const GLTANG_Type * type, GLTANG_TypeOp op) {
  return (type->operations >> (unsigned)op) & 1u;
}

#endif /* GHOTI_IO_GLTANG_LIBRARY_TYPES_INTERNAL_H */
