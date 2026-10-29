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
 * Continuation tables (type.h): the one place a table a host gives is checked
 * and copied, for a type's descriptor, a library member and a native made at run
 * time. A copy is one block: the entries, then their names.
 */

#ifndef GHOTI_IO_GLTANG_LIBRARY_CONTINUATIONS_H
#define GHOTI_IO_GLTANG_LIBRARY_CONTINUATIONS_H

#include <ghoti.io/lang-tang/macros.h>

#include <stddef.h>
#include <stdint.h>
#include <ghoti.io/lang-tang/type.h>

/**
 * @brief Whether a table is acceptable: 1 to ::GLTANG_CONTINUATIONS_MAX entries,
 *   each with a function and a name that is 1 to ::GLTANG_TYPE_NAME_MAX
 *   `[A-Za-z0-9_]` characters not starting with a digit, no name repeated.
 */
bool gltang_continuations_valid(const GLTANG_Continuation * table, size_t count);

/** @brief The size of the one block ::gltang_continuations_copy fills. */
size_t gltang_continuations_bytes(const GLTANG_Continuation * table, size_t count);

/** @brief Copies a valid table into `block` (at least ::gltang_continuations_bytes): the entries, then the names they point to. */
void gltang_continuations_copy(void * block, const GLTANG_Continuation * table, size_t count);

/** @brief Folds a table into a hash (index and name of each entry), as the table hash of a type does. */
uint64_t gltang_continuations_hash(uint64_t hash, const GLTANG_Continuation * table, size_t count);

#endif /* GHOTI_IO_GLTANG_LIBRARY_CONTINUATIONS_H */
