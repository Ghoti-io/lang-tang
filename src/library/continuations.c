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
 * Continuation tables: see continuations.h.
 */

#include <ghoti.io/lang-tang/macros.h>

#include <stdbool.h>
#include <string.h>
#include "continuations.h"

static bool valid_name(const char * name, size_t * length) {
  if (!name) {
    return false;
  }
  size_t n = 0;
  for (; name[n]; ++n) {
    char c = name[n];
    bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    bool digit = c >= '0' && c <= '9';
    if ((!letter && !(digit && n > 0)) || n >= GLTANG_TYPE_NAME_MAX) {
      return false;
    }
  }
  *length = n;
  return n > 0;
}

bool gltang_continuations_valid(const GLTANG_Continuation * table, size_t count) {
  if (!table || count == 0 || count > GLTANG_CONTINUATIONS_MAX) {
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    size_t length;
    if (!table[i].function || !valid_name(table[i].name, &length)) {
      return false;
    }
    for (size_t j = 0; j < i; ++j) {
      if (!strcmp(table[j].name, table[i].name)) {
        return false;
      }
    }
  }
  return true;
}

size_t gltang_continuations_bytes(const GLTANG_Continuation * table, size_t count) {
  size_t bytes = count * sizeof(GLTANG_Continuation);
  for (size_t i = 0; i < count; ++i) {
    bytes += strlen(table[i].name) + 1u;
  }
  return bytes;
}

void gltang_continuations_copy(void * block, const GLTANG_Continuation * table, size_t count) {
  GLTANG_Continuation * entries = block;
  char * names = (char *)block + count * sizeof(GLTANG_Continuation);
  for (size_t i = 0; i < count; ++i) {
    size_t n = strlen(table[i].name) + 1u;
    memcpy(names, table[i].name, n);
    entries[i].name = names;
    entries[i].function = table[i].function;
    names += n;
  }
}

static uint64_t fnv(uint64_t hash, const void * bytes, size_t length) {
  const unsigned char * p = bytes;
  for (size_t i = 0; i < length; ++i) {
    hash ^= p[i];
    hash *= UINT64_C(0x100000001b3);
  }
  return hash;
}

static uint64_t fnv_u64(uint64_t hash, uint64_t value) {
  unsigned char bytes[8];
  for (size_t i = 0; i < 8; ++i) {
    bytes[i] = (unsigned char)(value >> (8u * i));
  }
  return fnv(hash, bytes, sizeof bytes);
}

uint64_t gltang_continuations_hash(uint64_t hash, const GLTANG_Continuation * table, size_t count) {
  hash = fnv_u64(hash, count);
  for (size_t i = 0; i < count; ++i) {
    hash = fnv_u64(hash, i);
    hash = fnv_u64(hash, strlen(table[i].name));
    hash = fnv(hash, table[i].name, strlen(table[i].name));
  }
  return hash;
}
