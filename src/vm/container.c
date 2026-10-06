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
 * Arrays and maps: heap objects made of a header and a storage object, so that
 * growing replaces the storage and every name that refers to the container
 * sees the growth. Plus the deep copy and the equality that walk them.
 *
 * Every write to a slot the collector traces goes through grheap_store_word,
 * integers included (barrier-verify compares the slot with what the barrier
 * last wrote). A map keeps its entries in the order they were added and finds
 * them through an open-addressing index of 32-bit entry numbers; the index is
 * not traced, so it is written directly.
 */

#include <ghoti.io/lang-tang/macros.h>

#include <string.h>
#include "vm_internal.h"

/** @brief The most elements one array or map is allowed to ask for. */
#define MAX_ELEMENTS ((size_t)1 << 31)

static void put(GLTANG_Execution * exec, void * holder, GLTANG_Value * slot, GLTANG_Value v) {
  (void)grheap_store_word(exec->heap, holder, slot, v);
}

static void put_pointer(GLTANG_Execution * exec, void * holder, void ** slot, void * target) {
  (void)grheap_store(exec->heap, holder, slot, target);
}

typedef struct Pacer {
  GLTANG_Execution * exec;
  size_t since;
  GLTANG_NativeId native;  ///< Which native is paced, for the gate.
} Pacer;

static GLTANG_Status pace(Pacer * pacer, size_t elements) {
  // An element counts for the same as eight bytes of a string copy.
  pacer->since += elements * 8u;
  if (pacer->since >= GLTANG_POLL_BYTES) {
    size_t work = pacer->since / GLTANG_WORK_BYTES_PER_FUEL;
    pacer->since = 0;
    return gltang_vm_native_poll_as(pacer->exec, pacer->native, work ? work : 1);
  }
  return GLTANG_ST_OK;
}

// ---------------------------------------------------------------------------
// Arrays
// ---------------------------------------------------------------------------

static GLTANG_Status array_store_alloc(GLTANG_Execution * exec, size_t capacity, GLTANG_ArrayStore ** out) {
  if (capacity > MAX_ELEMENTS) {
    return GLTANG_ST_OOM;
  }
  void * object;
  GLTANG_Status st = gltang_vm_alloc(exec, &gltang_type_array_store, sizeof(GLTANG_ArrayStore) + capacity * sizeof(GLTANG_Value), &object);
  if (st != GLTANG_ST_OK) {
    return st;
  }
  GLTANG_ArrayStore * store = object;
  store->kind = GLTANG_OBJ_ARRAY_STORE;
  store->capacity = capacity;
  *out = store;
  return GLTANG_ST_OK;
}

GLTANG_Value gltang_vm_array_new(GLTANG_Execution * exec, size_t capacity) {
  void * object;
  GLTANG_Status st = gltang_vm_alloc(exec, &gltang_type_array, sizeof(GLTANG_ArrayObject), &object);
  if (st != GLTANG_ST_OK) {
    return gltang_vm_failure_value(exec, st);
  }
  GLTANG_ArrayObject * array = object;
  array->kind = GLTANG_OBJ_ARRAY;
  size_t mark = gltang_vm_temp_mark(exec);
  if (!gltang_vm_temp_push(exec, gltang_value_of(array))) {
    return exec->roots[GLTANG_ROOT_OOM];
  }
  GLTANG_ArrayStore * store;
  st = array_store_alloc(exec, capacity, &store);
  if (st != GLTANG_ST_OK) {
    gltang_vm_temp_release(exec, mark);
    return gltang_vm_failure_value(exec, st);
  }
  // The storage's allocation was a GC point: the array is where it is now.
  GLTANG_Value result = gltang_vm_temp_at(exec, mark);
  array = gltang_vm_array(result);
  put_pointer(exec, array, &array->store.raw, store);
  gltang_vm_temp_release(exec, mark);
  return result;
}

void gltang_vm_array_push(GLTANG_Execution * exec, GLTANG_Value array, GLTANG_Value v) {
  GLTANG_ArrayObject * a = gltang_vm_array(array);
  put(exec, a->store.typed, &a->store.typed->slots[a->length], v);
  ++a->length;
}

void gltang_vm_array_fill(GLTANG_Execution * exec, GLTANG_Value array, const GLTANG_Value * elements, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    gltang_vm_array_push(exec, array, elements[i]);
  }
}

void gltang_vm_array_set(GLTANG_Execution * exec, GLTANG_Value array, size_t index, GLTANG_Value v) {
  GLTANG_ArrayObject * a = gltang_vm_array(array);
  put(exec, a->store.typed, &a->store.typed->slots[index], v);
}

GLTANG_Value gltang_vm_array_grow(GLTANG_Execution * exec, GLTANG_Value array, size_t length) {
  GLTANG_ArrayObject * a = gltang_vm_array(array);
  if (length <= a->length) {
    return array;
  }
  if (length <= a->store.typed->capacity) {
    // The slots past the length are zero, which is null.
    a->length = length;
    return array;
  }
  if (length > MAX_ELEMENTS) {
    return exec->roots[GLTANG_ROOT_OOM];
  }
  size_t capacity = a->store.typed->capacity < 4 ? 4 : (size_t)a->store.typed->capacity * 2u;
  if (capacity < length) {
    capacity = length;
  }
  size_t count = (size_t)a->length;
  // The array is held across the allocation, and so is the new storage while
  // it is filled: each can move at a GC point, so each is read again from its
  // temporary (the array first, the storage next).
  size_t mark = gltang_vm_temp_mark(exec);
  if (!gltang_vm_temp_push(exec, array)) {
    return exec->roots[GLTANG_ROOT_OOM];
  }
  GLTANG_ArrayStore * store;
  GLTANG_Status st = array_store_alloc(exec, capacity, &store);
  if (st != GLTANG_ST_OK) {
    gltang_vm_temp_release(exec, mark);
    return gltang_vm_failure_value(exec, st);
  }
  // The new storage waits at polls while it is filled.
  if (!gltang_vm_temp_push(exec, gltang_value_of(store))) {
    gltang_vm_temp_release(exec, mark);
    return exec->roots[GLTANG_ROOT_OOM];
  }
  Pacer pacer = GLTANG_PACER(exec, ARRAY_GROW);
  GLTANG_Status poll = GLTANG_ST_OK;
  for (size_t i = 0; i < count && poll == GLTANG_ST_OK; ++i) {
    a = gltang_vm_array(gltang_vm_temp_at(exec, mark));
    store = (GLTANG_ArrayStore *)gltang_object(gltang_vm_temp_at(exec, mark + 1u));
    put(exec, store, &store->slots[i], a->store.typed->slots[i]);
    if ((i & 255u) == 255u) {
      poll = pace(&pacer, 256u);
    }
  }
  array = gltang_vm_temp_at(exec, mark);
  store = (GLTANG_ArrayStore *)gltang_object(gltang_vm_temp_at(exec, mark + 1u));
  gltang_vm_temp_release(exec, mark);
  if (poll != GLTANG_ST_OK) {
    return GLTANG_V_UNWIND;
  }
  a = gltang_vm_array(array);
  put_pointer(exec, a, &a->store.raw, store);
  a->length = length;
  return array;
}

// ---------------------------------------------------------------------------
// Maps
// ---------------------------------------------------------------------------

static size_t map_index_size(size_t capacity) {
  size_t size = 8;
  while (size < capacity * 2u) {
    size *= 2u;
  }
  return size;
}

static GLTANG_Status map_store_alloc(GLTANG_Execution * exec, size_t capacity, GLTANG_MapStore ** out) {
  if (capacity > MAX_ELEMENTS) {
    return GLTANG_ST_OOM;
  }
  size_t index_size = map_index_size(capacity);
  void * object;
  GLTANG_Status st = gltang_vm_alloc(exec, &gltang_type_map_store, sizeof(GLTANG_MapStore) + capacity * 2u * sizeof(GLTANG_Value) + index_size * sizeof(uint32_t), &object);
  if (st != GLTANG_ST_OK) {
    return st;
  }
  GLTANG_MapStore * store = object;
  store->kind = GLTANG_OBJ_MAP_STORE;
  store->capacity = capacity;
  store->index_size = index_size;
  *out = store;
  return GLTANG_ST_OK;
}

GLTANG_Value gltang_vm_map_new(GLTANG_Execution * exec, size_t capacity) {
  void * object;
  GLTANG_Status st = gltang_vm_alloc(exec, &gltang_type_map, sizeof(GLTANG_MapObject), &object);
  if (st != GLTANG_ST_OK) {
    return gltang_vm_failure_value(exec, st);
  }
  GLTANG_MapObject * map = object;
  map->kind = GLTANG_OBJ_MAP;
  size_t mark = gltang_vm_temp_mark(exec);
  if (!gltang_vm_temp_push(exec, gltang_value_of(map))) {
    return exec->roots[GLTANG_ROOT_OOM];
  }
  GLTANG_MapStore * store;
  st = map_store_alloc(exec, capacity, &store);
  if (st != GLTANG_ST_OK) {
    gltang_vm_temp_release(exec, mark);
    return gltang_vm_failure_value(exec, st);
  }
  // The storage's allocation was a GC point: the map is where it is now.
  GLTANG_Value result = gltang_vm_temp_at(exec, mark);
  map = gltang_vm_map(result);
  put_pointer(exec, map, &map->store.raw, store);
  gltang_vm_temp_release(exec, mark);
  return result;
}

/** The entry holding `key`, or -1. */
static int64_t map_find(const GLTANG_MapStore * store, const char * key, size_t length, uint64_t hash) {
  const uint32_t * index = gltang_map_index(store);
  size_t mask = (size_t)store->index_size - 1u;
  for (size_t i = (size_t)hash & mask;; i = (i + 1u) & mask) {
    uint32_t entry = index[i];
    if (!entry) {
      return -1;
    }
    const GLTANG_StringBlock * k = gltang_vm_string(store->slots[2u * (entry - 1u)]);
    if (k->byte_length == length && !memcmp(gltang_string_bytes(k), key, length)) {
      return (int64_t)entry - 1;
    }
  }
}

static void map_index_insert(GLTANG_MapStore * store, uint64_t hash, uint32_t entry_number) {
  uint32_t * index = gltang_map_index(store);
  size_t mask = (size_t)store->index_size - 1u;
  size_t i = (size_t)hash & mask;
  while (index[i]) {
    i = (i + 1u) & mask;
  }
  index[i] = entry_number;
}

/** Appends an entry known to be new and known to fit. */
static void map_append(GLTANG_Execution * exec, GLTANG_MapObject * map, GLTANG_Value key, GLTANG_Value v) {
  GLTANG_MapStore * store = map->store.typed;
  const GLTANG_StringBlock * k = gltang_vm_string(key);
  uint64_t n = map->count;
  put(exec, store, &store->slots[2u * n], key);
  put(exec, store, &store->slots[2u * n + 1u], v);
  map_index_insert(store, gltang_vm_string_hash(gltang_string_bytes(k), (size_t)k->byte_length), (uint32_t)(n + 1u));
  ++map->count;
}

GLTANG_Value gltang_vm_map_get(GLTANG_Value map, const char * key, size_t length, bool * found) {
  const GLTANG_MapObject * m = gltang_vm_map(map);
  int64_t entry = map_find(m->store.typed, key, length, gltang_vm_string_hash(key, length));
  *found = entry >= 0;
  return entry >= 0 ? m->store.typed->slots[2 * entry + 1] : GLTANG_V_NULL;
}

GLTANG_Value gltang_vm_map_set(GLTANG_Execution * exec, GLTANG_Value map, GLTANG_Value key, GLTANG_Value v) {
  GLTANG_MapObject * m = gltang_vm_map(map);
  const GLTANG_StringBlock * k = gltang_vm_string(key);
  const char * bytes = gltang_string_bytes(k);
  size_t length = (size_t)k->byte_length;
  uint64_t hash = gltang_vm_string_hash(bytes, length);
  int64_t entry = map_find(m->store.typed, bytes, length, hash);
  if (entry >= 0) {
    put(exec, m->store.typed, &m->store.typed->slots[2 * entry + 1], v);
    return map;
  }
  if (m->count == m->store.typed->capacity) {
    size_t capacity = m->store.typed->capacity < 4 ? 4 : (size_t)m->store.typed->capacity * 2u;
    // The allocation may collect, and the map, the key and the value are held
    // only by the caller's C variables: hold them, and take them from here
    // afterwards, since the collection may have moved them.
    size_t mark = gltang_vm_temp_mark(exec);
    if (!gltang_vm_temp_push(exec, map) || !gltang_vm_temp_push(exec, key) || !gltang_vm_temp_push(exec, v)) {
      gltang_vm_temp_release(exec, mark);
      return exec->roots[GLTANG_ROOT_OOM];
    }
    GLTANG_MapStore * store;
    GLTANG_Status st = map_store_alloc(exec, capacity, &store);
    map = gltang_vm_temp_at(exec, mark);
    key = gltang_vm_temp_at(exec, mark + 1u);
    v = gltang_vm_temp_at(exec, mark + 2u);
    gltang_vm_temp_release(exec, mark);
    if (st != GLTANG_ST_OK) {
      return gltang_vm_failure_value(exec, st);
    }
    m = gltang_vm_map(map);
    GLTANG_MapStore * old = m->store.typed;
    for (uint64_t i = 0; i < m->count; ++i) {
      put(exec, store, &store->slots[2u * i], old->slots[2u * i]);
      put(exec, store, &store->slots[2u * i + 1u], old->slots[2u * i + 1u]);
      const GLTANG_StringBlock * ok = gltang_vm_string(old->slots[2u * i]);
      map_index_insert(store, gltang_vm_string_hash(gltang_string_bytes(ok), (size_t)ok->byte_length), (uint32_t)(i + 1u));
    }
    put_pointer(exec, m, &m->store.raw, store);
  }
  map_append(exec, m, key, v);
  return map;
}

void gltang_vm_map_fill(GLTANG_Execution * exec, GLTANG_Value map, const GLTANG_Value * pairs, size_t count) {
  GLTANG_MapObject * m = gltang_vm_map(map);
  for (size_t i = 0; i < count; ++i) {
    GLTANG_Value key = pairs[2u * i];
    GLTANG_Value v = pairs[2u * i + 1u];
    const GLTANG_StringBlock * k = gltang_vm_string(key);
    const char * bytes = gltang_string_bytes(k);
    size_t length = (size_t)k->byte_length;
    uint64_t hash = gltang_vm_string_hash(bytes, length);
    int64_t entry = map_find(m->store.typed, bytes, length, hash);
    if (entry >= 0) {
      // Duplicate keys: the last wins.
      put(exec, m->store.typed, &m->store.typed->slots[2 * entry + 1], v);
    }
    else {
      map_append(exec, m, key, v);
    }
  }
}

// ---------------------------------------------------------------------------
// The deep copy
// ---------------------------------------------------------------------------

enum { COPY_OK = 0, COPY_OOM, COPY_UNWIND, COPY_DEEP };

static GLTANG_Value copy_rec(GLTANG_Execution * exec, GLTANG_Value v, int depth, int * fail, Pacer * pacer) {
  if (!gltang_vm_is_container(v)) {
    return v;
  }
  if (depth >= GLTANG_MAX_VALUE_DEPTH) {
    *fail = COPY_DEEP;
    return GLTANG_V_NULL;
  }
  bool is_array = gltang_v_is_kind(v, GLTANG_OBJ_ARRAY);
  size_t n = is_array ? (size_t)gltang_vm_array(v)->length : (size_t)gltang_vm_map(v)->count;
  // Every call below is a GC point, and the source is in no root of its own
  // here: hold it, with the copy, and read both again from the temporaries.
  size_t mark = gltang_vm_temp_mark(exec);
  if (!gltang_vm_temp_push(exec, v)) {
    *fail = COPY_OOM;
    return GLTANG_V_NULL;
  }
  GLTANG_Value copy = is_array ? gltang_vm_array_new(exec, n) : gltang_vm_map_new(exec, n);
  if (!gltang_v_is_kind(copy, is_array ? GLTANG_OBJ_ARRAY : GLTANG_OBJ_MAP)) {
    *fail = copy == GLTANG_V_UNWIND ? COPY_UNWIND : COPY_OOM;
    gltang_vm_temp_release(exec, mark);
    return GLTANG_V_NULL;
  }
  if (!gltang_vm_temp_push(exec, copy)) {
    *fail = COPY_OOM;
    gltang_vm_temp_release(exec, mark);
    return GLTANG_V_NULL;
  }
  for (size_t i = 0; i < n && !*fail; ++i) {
    GLTANG_Value source = gltang_vm_temp_at(exec, mark);
    if (is_array) {
      GLTANG_Value child = copy_rec(exec, gltang_vm_array(source)->store.typed->slots[i], depth + 1, fail, pacer);
      if (!*fail) {
        // Nothing collects between the child's return and its being stored.
        gltang_vm_array_push(exec, gltang_vm_temp_at(exec, mark + 1u), child);
      }
    }
    else {
      GLTANG_Value child = copy_rec(exec, gltang_vm_map(source)->store.typed->slots[2u * i + 1u], depth + 1, fail, pacer);
      if (!*fail) {
        source = gltang_vm_temp_at(exec, mark);
        GLTANG_Value pair[2] = {gltang_vm_map(source)->store.typed->slots[2u * i], child};
        gltang_vm_map_fill(exec, gltang_vm_temp_at(exec, mark + 1u), pair, 1);
      }
    }
    if (!*fail && pace(pacer, 1) != GLTANG_ST_OK) {
      *fail = COPY_UNWIND;
    }
  }
  copy = gltang_vm_temp_at(exec, mark + 1u);
  gltang_vm_temp_release(exec, mark);
  return copy;
}

GLTANG_Value gltang_vm_deep_copy(GLTANG_Execution * exec, GLTANG_Value v) {
  if (!gltang_vm_is_container(v)) {
    return v;
  }
  int fail = COPY_OK;
  Pacer pacer = GLTANG_PACER(exec, DEEP_COPY);
  GLTANG_Value copy = copy_rec(exec, v, 0, &fail, &pacer);
  switch (fail) {
    case COPY_OK: return copy;
    case COPY_UNWIND: return GLTANG_V_UNWIND;
    case COPY_DEEP: return gltang_vm_make_error(exec, GLTANG_ERROR_RECURSION_LIMIT);
    default: return exec->roots[GLTANG_ROOT_OOM];
  }
}

GLTANG_Value gltang_vm_op_adopt(GLTANG_Execution * exec, GLTANG_Value v) {
  return gltang_vm_deep_copy(exec, v);
}

// ---------------------------------------------------------------------------
// Equality
// ---------------------------------------------------------------------------

typedef enum { EQ_FALSE = 0, EQ_TRUE, EQ_NOT_SUPPORTED, EQ_DEEP, EQ_UNWIND, EQ_OOM } EqResult;

static bool is_weak(GLTANG_ValueKind k) {
  return k == GLTANG_KIND_NULL || k == GLTANG_KIND_BOOL || k == GLTANG_KIND_STRING;
}

static EqResult equal_rec(GLTANG_Value a, GLTANG_Value b, int depth, Pacer * pacer) {
  GLTANG_ValueKind ka = gltang_vm_kind(a);
  GLTANG_ValueKind kb = gltang_vm_kind(b);
  // Null, booleans and strings are equal to their own kind and to nothing
  // else, whatever the other side is (language reference, 13.3).
  if (is_weak(ka) || is_weak(kb)) {
    if (ka != kb) {
      return EQ_FALSE;
    }
    switch (ka) {
      case GLTANG_KIND_NULL: return EQ_TRUE;
      case GLTANG_KIND_BOOL: return a == b ? EQ_TRUE : EQ_FALSE;
      default: return gltang_vm_string_equal(a, b) ? EQ_TRUE : EQ_FALSE;
    }
  }
  if (gltang_vm_is_number(a) && gltang_vm_is_number(b)) {
    if (ka == GLTANG_KIND_INTEGER && kb == GLTANG_KIND_INTEGER) {
      return gltang_vm_int(a) == gltang_vm_int(b) ? EQ_TRUE : EQ_FALSE;
    }
    return gltang_vm_number(a) == gltang_vm_number(b) ? EQ_TRUE : EQ_FALSE;
  }
  if (ka == GLTANG_KIND_ARRAY && kb == GLTANG_KIND_ARRAY) {
    if (depth >= GLTANG_MAX_VALUE_DEPTH) {
      return EQ_DEEP;
    }
    if (gltang_vm_array(a)->length != gltang_vm_array(b)->length) {
      return EQ_FALSE;
    }
    uint64_t length = gltang_vm_array(a)->length;
    // The comparison polls as it goes, and a poll can move either array: hold
    // both, and take each element from the arrays as they are at that point.
    GLTANG_Execution * exec = pacer->exec;
    size_t mark = gltang_vm_temp_mark(exec);
    if (!gltang_vm_temp_push(exec, a) || !gltang_vm_temp_push(exec, b)) {
      gltang_vm_temp_release(exec, mark);
      return EQ_OOM;
    }
    EqResult result = EQ_TRUE;
    for (uint64_t i = 0; i < length; ++i) {
      GLTANG_Value ea = gltang_vm_array(gltang_vm_temp_at(exec, mark))->store.typed->slots[i];
      GLTANG_Value eb = gltang_vm_array(gltang_vm_temp_at(exec, mark + 1u))->store.typed->slots[i];
      EqResult r = equal_rec(ea, eb, depth + 1, pacer);
      if (r == EQ_DEEP || r == EQ_UNWIND || r == EQ_OOM) {
        result = r;
        break;
      }
      if (r != EQ_TRUE) {
        // An element that cannot be compared is not equal.
        result = EQ_FALSE;
        break;
      }
      if (pace(pacer, 1) != GLTANG_ST_OK) {
        result = EQ_UNWIND;
        break;
      }
    }
    gltang_vm_temp_release(exec, mark);
    return result;
  }
  return EQ_NOT_SUPPORTED;
}

GLTANG_Value gltang_vm_equal(GLTANG_Execution * exec, GLTANG_Value a, GLTANG_Value b) {
  Pacer pacer = GLTANG_PACER(exec, EQUALITY);
  switch (equal_rec(a, b, 0, &pacer)) {
    case EQ_TRUE: return GLTANG_V_TRUE;
    case EQ_FALSE: return GLTANG_V_FALSE;
    case EQ_DEEP: return gltang_vm_make_error(exec, GLTANG_ERROR_RECURSION_LIMIT);
    case EQ_UNWIND: return GLTANG_V_UNWIND;
    case EQ_OOM: return exec->roots[GLTANG_ROOT_OOM];
    default: return gltang_vm_make_error(exec, GLTANG_ERROR_NOT_SUPPORTED);
  }
}
