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
 * The type registry a host builds (type.h): create, add descriptors, seal,
 * bind, share.
 *
 * A registry is context-free and lives in the process allocator, as a library
 * does: it outlives every execution that attaches it, and is freed by its last
 * reference from any thread.
 */

#include <ghoti.io/lang-tang/macros.h>

#include <string.h>
#include <ghoti.io/cutil/memory.h>
#include "continuations.h"
#include "types_internal.h"

/** The id of the next registry: process-wide, counted up, never reused. */
static atomic_uint_fast64_t next_registry_id = 1;

/** Registries freed so far in the process: read by the tests only (test_hooks.h), to see when a registry went. */
static atomic_uint_fast64_t registries_freed = 0;

/* For the tests only; declared in vm/test_hooks.h. */
uint64_t gltang_vm_test_registries_freed(void) {
  return (uint64_t)atomic_load_explicit(&registries_freed, memory_order_acquire);
}

GLTANG_Result gltang_type_registry_create(GLTANG_TypeRegistry ** out_registry) {
  if (!out_registry) {
    return GLTANG_ERR_INVALID;
  }
  GLTANG_TypeRegistry * registry = gcu_calloc(1, sizeof(GLTANG_TypeRegistry));
  if (!registry) {
    return GLTANG_ERR_OOM;
  }
  atomic_init(&registry->references, 1);
  atomic_init(&registry->sealed, false);
  atomic_init(&registry->bound, 0);
  registry->id = (uint64_t)atomic_fetch_add_explicit(&next_registry_id, 1, memory_order_relaxed);
  *out_registry = registry;
  return GLTANG_OK;
}

GLTANG_TypeRegistry * gltang_type_registry_retain(GLTANG_TypeRegistry * registry) {
  if (registry) {
    atomic_fetch_add_explicit(&registry->references, 1, memory_order_relaxed);
  }
  return registry;
}

void gltang_type_registry_release(GLTANG_TypeRegistry * registry) {
  if (!registry) {
    return;
  }
  // acq_rel, as for a library: every other thread's reads of the registry
  // happen before the free. The last reference can be an instance's, let go by
  // the heap's destroy hook after its finalizer (custom.c), so this is plain C
  // and frees with the process allocator the registry was made with, never a
  // context's.
  if (atomic_fetch_sub_explicit(&registry->references, 1, memory_order_acq_rel) == 1) {
    for (size_t i = 0; i < registry->count; ++i) {
      gcu_free(registry->types[i]->continuations);
      gcu_free(registry->types[i]->weak);
      gcu_free(registry->types[i]);
    }
    gcu_free(registry->types);
    gcu_free(registry->attributes);
    gcu_free(registry);
    atomic_fetch_add_explicit(&registries_freed, 1, memory_order_release);
  }
}

bool gltang_type_registry_sealed(const GLTANG_TypeRegistry * registry) {
  return registry && atomic_load_explicit(&((GLTANG_TypeRegistry *)(uintptr_t)registry)->sealed, memory_order_acquire);
}

uint64_t gltang_type_registry_id(const GLTANG_TypeRegistry * registry) {
  return registry ? registry->id : 0;
}

size_t gltang_type_registry_count(const GLTANG_TypeRegistry * registry) {
  return registry ? registry->count : 0;
}

/** A name a type may have: 1 to 64 of `[A-Za-z0-9_]`, not starting with a digit. */
static bool valid_name(const char * name, size_t * out_length) {
  if (!name) {
    return false;
  }
  size_t n = 0;
  for (; name[n]; ++n) {
    char c = name[n];
    bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    bool digit = c >= '0' && c <= '9';
    if (!letter && !(digit && n > 0)) {
      return false;
    }
    if (n >= GLTANG_TYPE_NAME_MAX) {
      return false;
    }
  }
  *out_length = n;
  return n > 0;
}

const GLTANG_Type * gltang_type_registry_find(const GLTANG_TypeRegistry * registry, const char * name, uint32_t version) {
  if (!registry || !name) {
    return NULL;
  }
  for (size_t i = 0; i < registry->count; ++i) {
    const GLTANG_Type * t = registry->types[i];
    if (t->descriptor.version == version && !strcmp(t->name, name)) {
      return t;
    }
  }
  return NULL;
}

/** FNV-1a, 64 bits, over `length` bytes, continuing from `hash`. */
static uint64_t fnv(uint64_t hash, const void * bytes, size_t length) {
  const unsigned char * p = bytes;
  for (size_t i = 0; i < length; ++i) {
    hash ^= p[i];
    hash *= UINT64_C(0x100000001b3);
  }
  return hash;
}

/** Folds a number in byte by byte, low byte first, so the hash is the same on every host. */
static uint64_t fnv_u64(uint64_t hash, uint64_t value) {
  unsigned char bytes[8];
  for (size_t i = 0; i < 8; ++i) {
    bytes[i] = (unsigned char)(value >> (8u * i));
  }
  return fnv(hash, bytes, sizeof bytes);
}

/** The table hash: what the type declares, never an address (AD-29). */
static uint64_t table_hash(const GLTANG_Type * t) {
  uint64_t h = UINT64_C(0xcbf29ce484222325);
  h = fnv(h, t->name, t->name_length);
  h = fnv_u64(h, t->name_length);
  h = fnv_u64(h, t->descriptor.version);
  h = fnv_u64(h, t->descriptor.slot_count);
  h = fnv_u64(h, t->descriptor.byte_count);
  h = fnv_u64(h, t->descriptor.thread_safe ? 1u : 0u);
  h = fnv_u64(h, t->operations);
  if (t->resumable || t->continuation_count) {
    // Only a type that declares guest calls folds them in: one that does not hashes as it always did.
    h = fnv_u64(h, t->resumable);
    h = gltang_continuations_hash(h, t->continuations, t->continuation_count);
  }
  if (t->value_type || t->finalizable || t->weak_count) {
    // The collector's members, likewise only when declared: the semantics, whether there is a finalizer (its
    // address is not a name), and each weak slot's index in increasing order. A `copy` is an operation, so it is in
    // `operations` above.
    h = fnv_u64(h, UINT64_C(0x636f6c6c));  // "coll": keeps this run of numbers apart from the guest calls'
    h = fnv_u64(h, t->value_type ? 1u : 0u);
    h = fnv_u64(h, t->finalizable ? 1u : 0u);
    h = fnv_u64(h, t->weak_count);
    for (uint32_t i = 0; i < t->descriptor.slot_count; ++i) {
      if (gltang_type_weak_slot(t, i)) {
        h = fnv_u64(h, i);
      }
    }
  }
  return h;
}

static uint32_t operations_of(const GLTANG_TypeDescriptor * d) {
  uint32_t bits = 0;
#define HAS(member, op) bits |= d->member ? 1u << (unsigned)(op) : 0u
  HAS(add, GLTANG_TYPE_OP_ADD);
  HAS(subtract, GLTANG_TYPE_OP_SUBTRACT);
  HAS(multiply, GLTANG_TYPE_OP_MULTIPLY);
  HAS(divide, GLTANG_TYPE_OP_DIVIDE);
  HAS(modulo, GLTANG_TYPE_OP_MODULO);
  HAS(negate, GLTANG_TYPE_OP_NEGATE);
  HAS(less, GLTANG_TYPE_OP_LESS);
  HAS(less_equal, GLTANG_TYPE_OP_LESS_EQUAL);
  HAS(greater, GLTANG_TYPE_OP_GREATER);
  HAS(greater_equal, GLTANG_TYPE_OP_GREATER_EQUAL);
  HAS(equal, GLTANG_TYPE_OP_EQUAL);
  HAS(not_equal, GLTANG_TYPE_OP_NOT_EQUAL);
  HAS(attribute_get, GLTANG_TYPE_OP_ATTRIBUTE_GET);
  HAS(attribute_set, GLTANG_TYPE_OP_ATTRIBUTE_SET);
  HAS(index_get, GLTANG_TYPE_OP_INDEX_GET);
  HAS(index_set, GLTANG_TYPE_OP_INDEX_SET);
  HAS(slice, GLTANG_TYPE_OP_SLICE);
  HAS(iterator_begin, GLTANG_TYPE_OP_ITERATOR_BEGIN);
  HAS(iterator_next, GLTANG_TYPE_OP_ITERATOR_NEXT);
  HAS(call, GLTANG_TYPE_OP_CALL);
  HAS(cast, GLTANG_TYPE_OP_CAST);
  HAS(truthy, GLTANG_TYPE_OP_TRUTHY);
  HAS(print, GLTANG_TYPE_OP_PRINT);
  HAS(to_string, GLTANG_TYPE_OP_TO_STRING);
  HAS(copy, GLTANG_TYPE_OP_COPY);
#undef HAS
  return bits;
}

GLTANG_Result gltang_type_registry_add(GLTANG_TypeRegistry * registry, const GLTANG_TypeDescriptor * descriptor, const GLTANG_Type ** out_type) {
  if (!registry || !descriptor || gltang_type_registry_sealed(registry)) {
    return GLTANG_ERR_INVALID;
  }
  if (descriptor->size < GLTANG_TYPE_DESCRIPTOR_MIN_SIZE) {
    return GLTANG_ERR_INVALID;
  }
  // The members this engine knows, as far as the host's `size` covers them;
  // the rest stay empty. A larger `size` (a later header) is read only as far
  // as this one goes.
  GLTANG_TypeDescriptor copy;
  memset(&copy, 0, sizeof copy);
  memcpy(&copy, descriptor, descriptor->size < sizeof copy ? descriptor->size : sizeof copy);
  copy.size = sizeof copy;
  size_t name_length = 0;
  if (!valid_name(copy.name, &name_length)) {
    return GLTANG_ERR_INVALID;
  }
  if (copy.slot_count > GLTANG_TYPE_SLOTS_MAX || copy.byte_count > GLTANG_TYPE_BYTES_MAX) {
    return GLTANG_ERR_INVALID;
  }
  if (gltang_type_registry_find(registry, copy.name, copy.version)) {
    return GLTANG_ERR_INVALID;
  }
  const uint32_t operations = operations_of(&copy);
  // Guest calls: the mask names implemented operations other than `truthy`, and a table to ask them with.
  if (copy.resumable_ops >> (unsigned)GLTANG_TYPE_OP_COUNT) {
    return GLTANG_ERR_INVALID;
  }
  if ((copy.resumable_ops & ~operations) || (copy.resumable_ops & (1u << (unsigned)GLTANG_TYPE_OP_TRUTHY)) || (copy.resumable_ops & (1u << (unsigned)GLTANG_TYPE_OP_COPY))) {
    return GLTANG_ERR_INVALID;
  }
  // The collector's members. A `copy` is a value type's only; a value type with a finalizer must have one, since the
  // default copy would duplicate what the bytes name and the finalizer would release it twice.
  if ((unsigned)copy.copy_semantics > (unsigned)GLTANG_COPY_VALUE) {
    return GLTANG_ERR_INVALID;
  }
  const bool value_type = copy.copy_semantics == GLTANG_COPY_VALUE;
  if ((copy.copy && !value_type) || (value_type && copy.finalizer && !copy.copy)) {
    return GLTANG_ERR_INVALID;
  }
  if (copy.weak_slot_count && !copy.weak_slots) {
    return GLTANG_ERR_INVALID;
  }
  const size_t weak_count = copy.weak_slots ? copy.weak_slot_count : 0u;
  if (weak_count > copy.slot_count) {
    return GLTANG_ERR_INVALID;   // more indices than slots: one is past the end or listed twice
  }
  const bool has_table = copy.continuations != NULL || copy.continuation_count != 0;
  if ((copy.resumable_ops || has_table) && !gltang_continuations_valid(copy.continuations, copy.continuation_count)) {
    return GLTANG_ERR_INVALID;
  }
  if (registry->count == registry->capacity) {
    size_t grown = registry->capacity ? registry->capacity * 2u : 8u;
    GLTANG_Type ** bigger = gcu_realloc(registry->types, grown * sizeof(*bigger));
    if (!bigger) {
      return GLTANG_ERR_OOM;
    }
    registry->types = bigger;
    registry->capacity = grown;
  }
  // The weak slots as one bit per slot: checked here (each below the slot count, none twice), and read by the
  // slot calls and the copy without a search.
  uint8_t * weak = NULL;
  if (weak_count) {
    weak = gcu_calloc(((size_t)copy.slot_count + 7u) / 8u, 1);
    if (!weak) {
      return GLTANG_ERR_OOM;
    }
    for (size_t i = 0; i < weak_count; ++i) {
      uint32_t index = copy.weak_slots[i];
      if (index >= copy.slot_count || ((weak[index >> 3] >> (index & 7u)) & 1u)) {
        gcu_free(weak);
        return GLTANG_ERR_INVALID;
      }
      weak[index >> 3] = (uint8_t)(weak[index >> 3] | (1u << (index & 7u)));
    }
  }
  GLTANG_Type * type = gcu_calloc(1, sizeof(GLTANG_Type));
  if (!type) {
    gcu_free(weak);
    return GLTANG_ERR_OOM;
  }
  type->registry = registry;
  type->registry_id = registry->id;
  memcpy(type->name, copy.name, name_length);
  type->name[name_length] = '\0';
  type->name_length = name_length;
  type->descriptor = copy;
  type->descriptor.name = type->name;
  type->operations = operations;
  type->resumable = copy.resumable_ops;
  type->weak = weak;
  type->weak_count = (uint32_t)weak_count;
  type->value_type = value_type;
  type->finalizable = copy.finalizer != NULL;
  type->descriptor.weak_slots = NULL;   // the caller's list is not kept; the bits are the copy
  if (has_table) {
    void * block = gcu_calloc(1, gltang_continuations_bytes(copy.continuations, copy.continuation_count));
    if (!block) {
      gcu_free(weak);
      gcu_free(type);
      return GLTANG_ERR_OOM;
    }
    gltang_continuations_copy(block, copy.continuations, copy.continuation_count);
    type->continuations = block;
    type->continuation_count = copy.continuation_count;
    type->descriptor.continuations = type->continuations;
  }
  type->table_hash = table_hash(type);
  registry->types[registry->count++] = type;
  registry->bindable = registry->bindable || !copy.thread_safe;
  if (out_type) {
    *out_type = type;
  }
  return GLTANG_OK;
}

const GLTANG_HostAttribute * gltang_type_registry_find_attribute(const GLTANG_TypeRegistry * registry, GLTANG_ValueKind kind, const char * name, size_t length) {
  for (size_t i = 0; i < registry->attribute_count; ++i) {
    const GLTANG_HostAttribute * a = &registry->attributes[i];
    if (a->kind == kind && a->name_length == length && !memcmp(a->name, name, length)) {
      return a;
    }
  }
  return NULL;
}

GLTANG_Result gltang_type_registry_add_attribute(GLTANG_TypeRegistry * registry, GLTANG_ValueKind kind, const char * name, GLTANG_HostAttributeFn fn, void * user) {
  if (!registry || !fn || gltang_type_registry_sealed(registry)) {
    return GLTANG_ERR_INVALID;
  }
  // Maps and libraries are refused: a map's names are its keys and a library's
  // are its members, so a host attribute would shadow the program's own data.
  // A custom value's type has its own attribute_get.
  switch (kind) {
    case GLTANG_KIND_INTEGER:
    case GLTANG_KIND_FLOAT:
    case GLTANG_KIND_BOOL:
    case GLTANG_KIND_STRING:
    case GLTANG_KIND_ARRAY:
    case GLTANG_KIND_FUNCTION:
    case GLTANG_KIND_RNG:
      break;
    default:
      return GLTANG_ERR_INVALID;
  }
  size_t length = 0;
  if (!valid_name(name, &length)) {
    return GLTANG_ERR_INVALID;
  }
  if (gltang_type_registry_find_attribute(registry, kind, name, length)) {
    return GLTANG_ERR_INVALID;
  }
  if (registry->attribute_count == registry->attribute_capacity) {
    size_t grown = registry->attribute_capacity ? registry->attribute_capacity * 2u : 4u;
    GLTANG_HostAttribute * bigger = gcu_realloc(registry->attributes, grown * sizeof(*bigger));
    if (!bigger) {
      return GLTANG_ERR_OOM;
    }
    registry->attributes = bigger;
    registry->attribute_capacity = grown;
  }
  GLTANG_HostAttribute * a = &registry->attributes[registry->attribute_count++];
  memset(a, 0, sizeof *a);
  a->kind = kind;
  a->name_length = length;
  memcpy(a->name, name, length);
  a->fn = fn;
  a->user = user;
  return GLTANG_OK;
}

GLTANG_TypeRegistry * gltang_type_registry_attach(GLTANG_TypeRegistry * registry) {
  if (!registry) {
    return NULL;
  }
  atomic_store_explicit(&registry->sealed, true, memory_order_release);
  return gltang_type_registry_retain(registry);
}

bool gltang_type_registry_bind(GLTANG_TypeRegistry * registry, uint64_t context_serial) {
  if (!registry->bindable) {
    return true;
  }
  uint_fast64_t expected = 0;
  if (atomic_compare_exchange_strong_explicit(&registry->bound, &expected, context_serial, memory_order_acq_rel, memory_order_acquire)) {
    return true;
  }
  return expected == context_serial;
}

const char * gltang_type_name(const GLTANG_Type * type) {
  return type ? type->name : NULL;
}

uint32_t gltang_type_version(const GLTANG_Type * type) {
  return type ? type->descriptor.version : 0;
}

const GLTANG_TypeRegistry * gltang_type_registry(const GLTANG_Type * type) {
  return type ? type->registry : NULL;
}

void * gltang_type_user(const GLTANG_Type * type) {
  return type ? type->descriptor.user : NULL;
}

uint32_t gltang_type_slot_count(const GLTANG_Type * type) {
  return type ? type->descriptor.slot_count : 0;
}

uint32_t gltang_type_byte_count(const GLTANG_Type * type) {
  return type ? type->descriptor.byte_count : 0;
}

uint64_t gltang_type_table_hash(const GLTANG_Type * type) {
  return type ? type->table_hash : 0;
}

bool gltang_type_same(const GLTANG_Type * a, const GLTANG_Type * b) {
  if (!a || !b) {
    return a == b;
  }
  // The whole tuple, never the address: two registries holding one name and
  // version hold two types (AD-29).
  return a->registry_id == b->registry_id && a->descriptor.version == b->descriptor.version &&
         a->name_length == b->name_length && !memcmp(a->name, b->name, a->name_length);
}
