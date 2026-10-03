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
 * @file compile.h
 * @stability free
 *
 * The compiler: a syntax tree to a program.
 *
 * Name resolution is static (AD-9, design.md): every top-level name gets a
 * slot in the program scope and every function-local name a slot in its
 * frame, so there is no dictionary lookup at run time and no closure. A tree
 * made by ::gltang_parse is the only input.
 */

#ifndef GHOTI_IO_GLTANG_COMPILE_H
#define GHOTI_IO_GLTANG_COMPILE_H

#include <ghoti.io/lang-tang/macros.h>

#include <ghoti.io/lang-tang/core.h>
#include <ghoti.io/lang-tang/parse.h>
#include <ghoti.io/lang-tang/program.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Compiles a tree.
 *
 * The tree is read and not kept: the program owns everything it needs, so the
 * tree may be destroyed as soon as this returns. The program is immutable.
 *
 * Errors that are not syntax errors are ::GLTANG_ERR_FORMAT with `error_out`
 * filled (line, column, message) and nothing built: `global` outside a
 * function; a function or a variable declared twice (including a name used
 * before a function of that name is declared, which is how a call to a
 * function declared later is refused); a repeated parameter name; and an
 * assignment target that is not a name, a member or a subscript (`Cannot
 * assign to this expression.`, a slice included).
 *
 * @param tree The tree, from ::gltang_parse. An empty tree compiles to an
 *   empty program.
 * @param file_name The name the program reports for its source, copied. NULL
 *   means "<program>".
 * @param error_out Optional (may be NULL). Written only when the result is
 *   ::GLTANG_ERR_FORMAT.
 * @param program_out Receives the program, with one reference. Written only on
 *   success; release it with ::gltang_program_release.
 * @return ::GLTANG_OK; ::GLTANG_ERR_FORMAT; ::GLTANG_ERR_LIMIT when the
 *   program is too large for the encoding (more than 16,777,215 instructions
 *   in a function, constants or locals) or the tree is deeper than
 *   ::GLTANG_MAX_TREE_DEPTH; ::GLTANG_ERR_OOM; ::GLTANG_ERR_INVALID for a NULL
 *   `tree` or `program_out`. A refusal builds nothing and frees everything it
 *   allocated.
 */
GLTANG_API GLTANG_Result gltang_compile(const GLTANG_Tree * tree,
    const char * file_name, GLTANG_ParseError * error_out,
    GLTANG_Program ** program_out);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GLTANG_COMPILE_H */
