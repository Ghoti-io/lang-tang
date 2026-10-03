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
 * The inside of a library: its members, and what the engine reads from them.
 *
 * A library is plain C memory (cutil's allocator, not a context's): the host
 * builds it before any context exists, and it outlives them all. It is
 * immutable once sealed, so the engine reads it from any thread with no lock.
 */

#ifndef GHOTI_IO_GLTANG_LIBRARY_LIBRARY_INTERNAL_H
#define GHOTI_IO_GLTANG_LIBRARY_LIBRARY_INTERNAL_H

#include <ghoti.io/lang-tang/macros.h>

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <ghoti.io/lang-tang/library.h>
#include <ghoti.io/lang-tang/seeds.h>

/** @brief What a member is. */
typedef enum {
  GLTANG_MEMBER_NULL = 0,
  GLTANG_MEMBER_BOOL,
  GLTANG_MEMBER_INTEGER,
  GLTANG_MEMBER_FLOAT,
  GLTANG_MEMBER_STRING,
  GLTANG_MEMBER_NATIVE,
  GLTANG_MEMBER_TEMPLATE,
  GLTANG_MEMBER_LIBRARY,
  GLTANG_MEMBER_FACTORY,
  GLTANG_MEMBER_BUILTIN   ///< Written in C inside the engine: see GLTANG_BuiltinId.
} GLTANG_MemberKind;

/** @brief The built-in members that need the engine (`random`'s). */
typedef enum {
  GLTANG_BUILTIN_NONE = 0,
  GLTANG_BUILTIN_RANDOM_GLOBAL,    ///< `random.global`: the execution's generator.
  GLTANG_BUILTIN_RANDOM_DEFAULT,   ///< `random.default`: a new generator on each access.
  GLTANG_BUILTIN_RANDOM_SEEDED,    ///< `random.seeded(n)`.
  GLTANG_BUILTIN_RNG_SET_SEED      ///< `rng.set_seed(n)`: the method of a generator.
} GLTANG_BuiltinId;

/** @brief One member of a library. */
typedef struct GLTANG_LibraryMember {
  char * name;                   ///< Owned (static for a built-in library).
  GLTANG_MemberKind kind;
  bool boolean;
  int64_t integer;
  double number;
  char * text;                   ///< STRING: owned, NUL-terminated.
  size_t length;
  GLTANG_String_Type encoding;
  GLTANG_NativeFn native;
  GLTANG_FactoryFn factory;
  void * user;
  GLTANG_Program * program;      ///< TEMPLATE: retained.
  uint64_t scope_fuel;
  GLTANG_ScopePolicy policy;
  GLTANG_Library * library;      ///< LIBRARY: retained.
  GLTANG_BuiltinId builtin;
} GLTANG_LibraryMember;

struct GLTANG_Library {
  atomic_size_t references;
  atomic_bool sealed;
  bool is_static;                ///< A built-in: never freed, always sealed.
  char * name;
  GLTANG_LibraryMember * members;
  size_t count;
  size_t capacity;
};

/** @brief The member of this name, or NULL. */
const GLTANG_LibraryMember * gltang_library_find(
    const GLTANG_Library * library, const char * name, size_t length);

/** @brief Seals a library (the library is attached somewhere). Takes a reference. */
GLTANG_Library * gltang_library_attach(GLTANG_Library * library);

/** @brief The built-in registry root: `math` and `random`. Static, sealed. */
const GLTANG_Library * gltang_library_builtins(void);

/** @brief The standard splitmix64 step: mixes `state` advanced by the golden gamma. */
uint64_t gltang_splitmix64_mix(uint64_t state);

/** @brief Operating-system entropy; false when there is none. */
bool gltang_entropy(void * bytes, size_t length);

#endif /* GHOTI_IO_GLTANG_LIBRARY_LIBRARY_INTERNAL_H */
