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
 * @file parse.h
 * @stability stable
 *
 * Parse Tang source into an owned syntax tree.
 *
 * The grammar, the scanner (template and script modes) and every string,
 * integer and date rule are ctang's, ported as this library's own source: the
 * accepted language is the one documented in ctang's language reference. What
 * differs is only the interface. ctang returned a tree whose root might be a
 * parse-error node, or NULL, and NULL meant both "nothing to parse" and "the
 * parser could not allocate". This returns a result and keeps the two apart.
 */

#ifndef GHOTI_IO_GLTANG_PARSE_H
#define GHOTI_IO_GLTANG_PARSE_H

#include <ghoti.io/lang-tang/macros.h>

#include <ghoti.io/lang-tang/core.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A node of the syntax tree.
 *
 * The node classes are declared in the `ast/` headers, which are labelled
 * `free`. This header only names the type, so that the stable interface does
 * not drag the unstable one in.
 */
typedef struct GLTANG_Ast_Node GLTANG_Ast_Node;

/** @brief How the source is read. */
typedef enum {
  GLTANG_PARSE_TEMPLATE, ///< Text with `<% code %>` and `<%= expr %>` tags.
  GLTANG_PARSE_SCRIPT,   ///< The whole source is code.
  GLTANG_PARSE_MODE_COUNT ///< Not a mode: closes the enum.
} GLTANG_ParseMode;

/** @brief The longest message ::GLTANG_ParseError keeps, terminator included. */
#define GLTANG_PARSE_ERROR_MESSAGE_SIZE 256

/**
 * @brief Where and why a parse was refused.
 *
 * Plain data: it owns nothing and may outlive the call. `line` and `column`
 * are 1-based (ctang's scanner counts from zero; the interface adds one) and
 * locate the token the parser refused, as the scanner placed it - for a string
 * literal, where the scanner finished reading it. The message is the parser's
 * own and is truncated, never overrun.
 */
typedef struct {
  int line;                                      ///< 1-based line.
  int column;                                    ///< 1-based column.
  char message[GLTANG_PARSE_ERROR_MESSAGE_SIZE]; ///< NUL-terminated text.
} GLTANG_ParseError;

/**
 * @brief A parsed source: the tree and the count of its nodes.
 *
 * Owned by the caller of gltang_parse() and released with
 * gltang_tree_destroy(), which frees every node. An empty source (nothing but
 * whitespace and comments) is a tree with no root.
 */
typedef struct GLTANG_Tree GLTANG_Tree;

/**
 * @brief Parse a NUL-terminated source.
 *
 * @param source The source text. Read, never retained.
 * @param mode ::GLTANG_PARSE_TEMPLATE or ::GLTANG_PARSE_SCRIPT.
 * @param error_out Optional (may be NULL). Written only when the result is
 *   ::GLTANG_ERR_FORMAT, with where and why; left untouched otherwise.
 * @param tree_out Receives the tree on ::GLTANG_OK, and is untouched on every
 *   other result. Release it with gltang_tree_destroy().
 * @return ::GLTANG_OK, including for an empty source; ::GLTANG_ERR_FORMAT for
 *   a syntax or scanner error (an unterminated string, a trailing backslash,
 *   an octal escape over 255, `%>` in script mode, a character that is not a
 *   token, invalid UTF-8 in a string or template text, an integer too large,
 *   `a[i] += 1`, truncated input); ::GLTANG_ERR_LIMIT when the parser's own
 *   stack limit is reached by nesting thousands deep (bison reports a stack it
 *   could not grow the same way, so a real out-of-memory there also reads as
 *   this result); ::GLTANG_ERR_OOM when an
 *   allocation fails; ::GLTANG_ERR_INVALID for a NULL `source` or `tree_out`
 *   or a bad mode. A refusal allocates nothing the caller must free.
 */
GLTANG_API GLTANG_Result gltang_parse(const char * source, GLTANG_ParseMode mode, GLTANG_ParseError * error_out, GLTANG_Tree ** tree_out);

/**
 * @brief Release a tree and every node in it.
 *
 * @param tree The tree. NULL is accepted and does nothing.
 */
GLTANG_API void gltang_tree_destroy(GLTANG_Tree * tree);

/**
 * @brief The number of nodes in the tree.
 *
 * The same walk ctang's node-count function made, which is what lets the
 * oracle compare two parses by count.
 *
 * @param tree The tree, or NULL.
 * @return The count; 0 for NULL or an empty tree.
 */
GLTANG_API size_t gltang_tree_node_count(const GLTANG_Tree * tree);

/**
 * @brief The root node.
 *
 * @param tree The tree, or NULL.
 * @return The root, borrowed from the tree and valid until it is destroyed;
 *   NULL for NULL or an empty tree.
 */
GLTANG_API GLTANG_Ast_Node * gltang_tree_root(const GLTANG_Tree * tree);

/**
 * @brief Print the tree to stdout, one node per line, indented by depth.
 *
 * This is the AST's own `print`, as ported: it writes to stdout. A tree with
 * no root prints nothing.
 *
 * @param tree The tree, or NULL.
 */
GLTANG_API void gltang_tree_print(const GLTANG_Tree * tree);

/**
 * @brief Count the nodes under (and including) a node.
 *
 * @param node A node, or NULL.
 * @return The count; 0 for NULL.
 */
GLTANG_API size_t gltang_ast_node_count(GLTANG_Ast_Node * node);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GLTANG_PARSE_H */
