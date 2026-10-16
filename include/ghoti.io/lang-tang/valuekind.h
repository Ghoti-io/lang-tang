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
 * @file valuekind.h
 * @stability stable
 *
 * What kind of value a result or a handle is. Its own header so that the
 * handle API (handle.h), which is stable, can name it without including
 * value.h, which is free; value.h includes it.
 */

#ifndef GHOTI_IO_GLTANG_VALUEKIND_H
#define GHOTI_IO_GLTANG_VALUEKIND_H

#include <ghoti.io/lang-tang/macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief What kind of value a result is. */
typedef enum {
  GLTANG_KIND_NULL = 0, ///< `null`, and the result of an empty program.
  GLTANG_KIND_BOOL,     ///< `true` or `false`.
  GLTANG_KIND_INTEGER,  ///< A signed 64-bit integer.
  GLTANG_KIND_FLOAT,    ///< An IEEE-754 double.
  GLTANG_KIND_STRING,   ///< A grapheme string with segment-tagged encodings.
  GLTANG_KIND_ARRAY,    ///< A mutable array.
  GLTANG_KIND_MAP,      ///< A mutable map from strings to values.
  GLTANG_KIND_FUNCTION, ///< A function declared by the program, a native function, or a template.
  GLTANG_KIND_ERROR,    ///< An error value (language reference, section 10.2).
  GLTANG_KIND_LIBRARY,  ///< A library: a named table of members (language reference, section 9).
  GLTANG_KIND_RNG       ///< A random number generator (`random.seeded(n)`).
} GLTANG_ValueKind;

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GLTANG_VALUEKIND_H */
