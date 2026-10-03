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
 * The built-in libraries, `math` and `random` (language reference, section 9),
 * written in C inside the engine: the third layer of a `use`'s resolution.
 *
 * They are static, sealed and never freed, so any number of executions on any
 * number of threads read them with no lock. `math` has `pi`; every other name
 * is `Not implemented`, as in ctang. `random` has `global`, `default` and
 * `seeded`; the members that need an execution are built-in members that the
 * engine implements (vm/libvalue.c).
 */

#include <ghoti.io/lang-tang/macros.h>

#include "library_internal.h"

#define MEMBER_FLOAT(n, v) {.name = (n), .kind = GLTANG_MEMBER_FLOAT, .number = (v)}
#define MEMBER_BUILTIN(n, id) {.name = (n), .kind = GLTANG_MEMBER_BUILTIN, .builtin = (id)}

static GLTANG_LibraryMember math_members[] = {
  MEMBER_FLOAT("pi", 3.14159265358979323846),
};

static GLTANG_LibraryMember random_members[] = {
  MEMBER_BUILTIN("global", GLTANG_BUILTIN_RANDOM_GLOBAL),
  MEMBER_BUILTIN("default", GLTANG_BUILTIN_RANDOM_DEFAULT),
  MEMBER_BUILTIN("seeded", GLTANG_BUILTIN_RANDOM_SEEDED),
};

static GLTANG_Library math_library = {
  .references = 1, .sealed = true, .is_static = true, .name = "math",
  .members = math_members, .count = sizeof(math_members) / sizeof(math_members[0]),
  .capacity = sizeof(math_members) / sizeof(math_members[0]),
};

static GLTANG_Library random_library = {
  .references = 1, .sealed = true, .is_static = true, .name = "random",
  .members = random_members, .count = sizeof(random_members) / sizeof(random_members[0]),
  .capacity = sizeof(random_members) / sizeof(random_members[0]),
};

static GLTANG_LibraryMember root_members[] = {
  {.name = "math", .kind = GLTANG_MEMBER_LIBRARY, .library = &math_library},
  {.name = "random", .kind = GLTANG_MEMBER_LIBRARY, .library = &random_library},
};

static GLTANG_Library root_library = {
  .references = 1, .sealed = true, .is_static = true, .name = NULL,
  .members = root_members, .count = sizeof(root_members) / sizeof(root_members[0]),
  .capacity = sizeof(root_members) / sizeof(root_members[0]),
};

const GLTANG_Library * gltang_library_builtins(void) {
  return &root_library;
}
