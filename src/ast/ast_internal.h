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
 * Private helpers of the syntax tree: the height each node records and the
 * refusal that bounds it (the tree-depth budget, GLTANG_MAX_TREE_DEPTH).
 */

#ifndef GHOTI_IO_GLTANG_AST_AST_INTERNAL_H
#define GHOTI_IO_GLTANG_AST_AST_INTERNAL_H

#include <ghoti.io/lang-tang/macros.h>

#include <stdbool.h>
#include <stdint.h>
#include <ghoti.io/lang-tang/ast/astNode.h>

/** The height of a child; a missing child counts for nothing. */
static inline uint32_t gltang_ast_height(const GLTANG_Ast_Node * node) {
  return node ? (node->depth ? node->depth : 1) : 0;
}

/** The larger of two heights. */
static inline uint32_t gltang_ast_height_max(uint32_t a, uint32_t b) {
  return a > b ? a : b;
}

/** The tallest node of a vector of node pointers. */
uint32_t gltang_ast_height_of_nodes(const GLTANG_VectorX * nodes);

/** The tallest key or value of a vector of map pairs. */
uint32_t gltang_ast_height_of_pairs(const GLTANG_VectorX * pairs);

/**
 * Whether a node of this height may exist. A height past the budget is
 * refused, and the refusal is remembered for gltang_parse() to turn into
 * ::GLTANG_ERR_LIMIT: a constructor can only return NULL, which the grammar
 * reads as running out of memory.
 */
bool gltang_ast_depth_ok(uint32_t height);

/** Clears the remembered refusal. */
void gltang_ast_depth_reset(void);

/** Whether a refusal was remembered since the last reset. */
bool gltang_ast_depth_refused(void);

/**
 * The most characters of indent a printed line carries. Past it the indent
 * stops growing, so printing a tree costs time and space linear in its size:
 * the line for a node at depth d used to be 4*d characters, and a printed
 * chain of 10,000 nodes was hundreds of megabytes.
 */
#define GLTANG_PRINT_INDENT_MAX 256

/**
 * The indent for the children of a node printed at `indent`: `indent` with
 * `extra` more spaces, in a new buffer, or `indent` itself once it is at the
 * cap (so no allocation and no copy). Release it with
 * gltang_ast_indent_release(), naming the same `indent`.
 *
 * @return The indent, or NULL if the buffer could not be allocated.
 */
char * gltang_ast_indent_extend(const char * indent, size_t extra);

/** Releases what gltang_ast_indent_extend() returned. */
void gltang_ast_indent_release(char * extended, const char * indent);

#endif /* GHOTI_IO_GLTANG_AST_AST_INTERNAL_H */
