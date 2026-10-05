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
 * @stability free
 *
 * This file defines the location types used by the lexers and parsers.
 */

#ifndef GHOTI_IO_GLTANG_LOCATION_H
#define GHOTI_IO_GLTANG_LOCATION_H

#include <ghoti.io/lang-tang/macros.h>

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

/**
 * A span of source text, as the parser reports it for a token or a rule.
 * Lines and columns are as the generated parser counts them.
 */
typedef struct GLTANG_PARSER_LTYPE
{
  int first_line;   ///< Line on which the span starts.
  int first_column; ///< Column at which the span starts.
  int last_line;    ///< Line on which the span ends.
  int last_column;  ///< Column at which the span ends.
} GLTANG_PARSER_LTYPE;

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // GHOTI_IO_GLTANG_LOCATION_H
