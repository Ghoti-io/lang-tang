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
 * gltang_parse(): the front end's one entry point.
 *
 * Ported from ctang's primary parse function, with the differences the
 * interface asks for. The failure policy is the same as ctang's after its
 * fix 13.1 - a parse that failed is never mistaken for an empty source - but
 * it is now a result, not a node the caller must know to look for.
 */

#include <ghoti.io/lang-tang/macros.h>

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <ghoti.io/cutil/memory.h>
#include <ghoti.io/lang-tang/parse.h>
#include <ghoti.io/lang-tang/tangScanner.h>

#define YY_HEADER_EXPORT_START_CONDITIONS
#include "flexTangScanner.h"
#undef YY_HEADER_EXPORT_START_CONDITIONS

#include <ghoti.io/lang-tang/ast/astNodeAll.h>
#include "../ast/ast_internal.h"

// Defined by the generated scanner and parser, which are not declared in any
// header of ours.
void gltang_flex_set_state(int state, void * yyscanner);
GLTANG_Parser_Error_Kind gltang_parser_error_kind(GLTANG_Parser_Error error);

struct GLTANG_Tree {
  GLTANG_Ast_Node * root; ///< NULL for an empty source.
  size_t node_count;      ///< The nodes under root.
};


static void count_nodes(GLTANG_MAYBE_UNUSED(GLTANG_Ast_Node * self), GLTANG_MAYBE_UNUSED(void * data), void * return_value) {
  assert(return_value);
  ++*(size_t *)return_value;
}


size_t gltang_ast_node_count(GLTANG_Ast_Node * node) {
  size_t count = 0;
  if (node) {
    gltang_ast_node_walk(node, count_nodes, 0, &count);
  }
  return count;
}


// bison reports "memory exhausted" both when its stack cannot grow and when
// the nesting passes its fixed maximum (10000 levels), and it cannot be told
// which. The second is the one a source can cause, so it is the one reported.
static bool is_stack_limit(const char * message) {
  return !strcmp(message, "memory exhausted");
}


// The scanner counts lines and columns from zero. The interface says 1-based,
// which is what an editor, a compiler message and the `tang` command show.
static void fill_error(GLTANG_ParseError * out, int line, int column, const char * message) {
  out->line = line + 1;
  out->column = column + 1;
  out->message[0] = '\0';
  if (message) {
    strncpy(out->message, message, GLTANG_PARSE_ERROR_MESSAGE_SIZE - 1);
    out->message[GLTANG_PARSE_ERROR_MESSAGE_SIZE - 1] = '\0';
  }
}


GLTANG_Result gltang_parse(const char * source, GLTANG_ParseMode mode, GLTANG_ParseError * error_out, GLTANG_Tree ** tree_out) {
  if (!source || !tree_out || (unsigned)mode >= (unsigned)GLTANG_PARSE_MODE_COUNT) {
    return GLTANG_ERR_INVALID;
  }

  // Everything that can fail for want of memory is reserved before the scan.
  size_t length = strlen(source);
  if (length > SIZE_MAX - GLTANG_SCAN_POOL_SLACK) {
    return GLTANG_ERR_OOM;
  }
  GLTANG_Tree * tree = gcu_malloc(sizeof(GLTANG_Tree));
  if (!tree) {
    return GLTANG_ERR_OOM;
  }
  char * pool_memory = gcu_malloc(length + GLTANG_SCAN_POOL_SLACK);
  if (!pool_memory) {
    gcu_free(tree);
    return GLTANG_ERR_OOM;
  }
  GLTANG_Scan_Pool pool = {
    .base = pool_memory,
    .used = 0,
    .capacity = length + GLTANG_SCAN_POOL_SLACK,
    .out_of_memory = false,
  };

  yyscan_t scanner;
  if (gltang_flexlex_init_extra(&pool, &scanner)) {
    gcu_free(pool_memory);
    gcu_free(tree);
    return GLTANG_ERR_OOM;
  }
  if (mode == GLTANG_PARSE_TEMPLATE) {
    gltang_flex_set_state(TEMPLATE, scanner);
  }

  gltang_ast_depth_reset();
  GLTANG_Ast_Node * ast = 0;
  GLTANG_Parser_Error parse_error = 0;
  GLTANG_PARSER_LTYPE error_location = {0, 0, 0, 0};
  YY_BUFFER_STATE state = gltang_flex_scan_string(source, scanner);
  GLTANG_Parser_parse(scanner, &ast, &parse_error, &error_location);
  gltang_flex_delete_buffer(state, scanner);
  gltang_flexlex_destroy(scanner);
  bool scanner_out_of_memory = pool.out_of_memory;
  gcu_free(pool_memory);

  // A failed parse must not come back as an empty tree. A rule action that
  // cannot allocate, or that is handed input it must reject, sets parse_error
  // itself and never reaches bison's error handler, so there may be no node.
  if (parse_error || scanner_out_of_memory) {
    GLTANG_Result result = GLTANG_ERR_FORMAT;
    GLTANG_Parser_Error_Kind kind = gltang_parser_error_kind(parse_error);
    if (scanner_out_of_memory || kind == GLTANG_PARSER_ERROR_KIND_OUT_OF_MEMORY) {
      result = GLTANG_ERR_OOM;
    }
    else if (ast && GLTANG_AST_IS_PARSE_ERROR(ast)) {
      // bison's error handler built a node: syntax or scanner error.
      const char * message = gltang_ast_node_parse_error_message(ast);
      if (is_stack_limit(message)) {
        result = GLTANG_ERR_LIMIT;
      }
      else if (error_out) {
        fill_error(error_out, ast->location.first_line, ast->location.first_column, message);
      }
    }
    else if (kind == GLTANG_PARSER_ERROR_KIND_INVALID_UTF8) {
      // A rule refused its input: there is no node, only the literal and the
      // place the rule recorded.
      if (error_out) {
        fill_error(error_out, error_location.first_line, error_location.first_column, parse_error);
      }
    }
    else {
      // The error handler ran but could not allocate its node.
      result = GLTANG_ERR_OOM;
    }
    // A constructor that refused a tree taller than the budget can only return
    // NULL, which the grammar reads as running out of memory. The refusal was
    // remembered, so the caller is told which it was.
    if (result == GLTANG_ERR_OOM && gltang_ast_depth_refused()) {
      result = GLTANG_ERR_LIMIT;
    }
    gltang_ast_depth_reset();
    if (ast) {
      gltang_ast_node_destroy(ast);
    }
    gcu_free(tree);
    return result;
  }

  tree->root = ast;
  tree->node_count = gltang_ast_node_count(ast);
  *tree_out = tree;
  return GLTANG_OK;
}


void gltang_tree_destroy(GLTANG_Tree * tree) {
  if (!tree) {
    return;
  }
  if (tree->root) {
    gltang_ast_node_destroy(tree->root);
  }
  gcu_free(tree);
}


size_t gltang_tree_node_count(const GLTANG_Tree * tree) {
  return tree ? tree->node_count : 0;
}


GLTANG_Ast_Node * gltang_tree_root(const GLTANG_Tree * tree) {
  return tree ? tree->root : 0;
}


void gltang_tree_print(const GLTANG_Tree * tree) {
  if (tree && tree->root) {
    gltang_ast_node_print(tree->root, "");
  }
}
