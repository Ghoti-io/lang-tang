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
 * Parse a template and print the number of nodes in its syntax tree, then show
 * what a refusal looks like.
 *
 * This is the whole of the stable interface at this stage: gltang_parse() turns
 * a NUL-terminated source into a tree the caller owns, or says where and why
 * it could not. Nothing here needs an engine; execution arrives with the
 * interpreter.
 */

#include <ghoti.io/lang-tang/lang-tang.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  /* A template: text, a code tag, and a print tag. */
  const char * source =
    "<h1><%= title %></h1>\n"
    "<% for (item : items) { %><li><%= item %></li>\n<% } %>";

  GLTANG_Tree * tree = NULL;
  GLTANG_Result result = gltang_parse(source, GLTANG_PARSE_TEMPLATE, NULL, &tree);
  if (result != GLTANG_OK) {
    fprintf(stderr, "parse failed: %s\n", gltang_result_string(result));
    return 1;
  }
  size_t nodes = gltang_tree_node_count(tree);
  printf("the template has %zu nodes\n", nodes);
  if (nodes == 0) {
    fprintf(stderr, "expected a non-empty tree\n");
    gltang_tree_destroy(tree);
    return 1;
  }
  gltang_tree_destroy(tree);

  /* A source with a syntax error: the result says FORMAT, the error says
   * where, and nothing is left for the caller to free. */
  GLTANG_ParseError error;
  result = gltang_parse("count = ;", GLTANG_PARSE_SCRIPT, &error, &tree);
  if (result != GLTANG_ERR_FORMAT) {
    fprintf(stderr, "expected a syntax error, got %s\n", gltang_result_string(result));
    return 1;
  }
  printf("refused at line %d, column %d: %s\n", error.line, error.column, error.message);
  if (error.line != 1 || error.column != 9) {
    fprintf(stderr, "the error is not where the semicolon is\n");
    return 1;
  }
  return 0;
}
