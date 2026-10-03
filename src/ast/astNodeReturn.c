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

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <ghoti.io/cutil/memory.h>
#include <ghoti.io/lang-tang/macros.h>
#include <ghoti.io/lang-tang/parse.h>
#include "ast_internal.h"
#include <ghoti.io/lang-tang/ast/astNodeReturn.h>

GLTANG_Ast_Node_VTable gltang_ast_node_return_vtable = {
  .name = "Return",
  .destroy = gltang_ast_node_return_destroy,
  .print = gltang_ast_node_return_print,
  .walk = gltang_ast_node_return_walk,
};


GLTANG_Ast_Node_Return * gltang_ast_node_return_create(GLTANG_Ast_Node * expression, GLTANG_PARSER_LTYPE location) {
  uint32_t depth = 1 + gltang_ast_height(expression);
  if (!gltang_ast_depth_ok(depth)) {
    return 0;
  }

  GLTANG_Ast_Node_Return * self = gcu_malloc(sizeof(GLTANG_Ast_Node_Return));
  if (!self) {
    return 0;
  }

  if (!expression) {
    // No expression was supplied.  Default to NULL.
    expression = gltang_ast_node_create(location);
    if (!expression) {
      gcu_free(self);
      return 0;
    }
  }

  *self = (GLTANG_Ast_Node_Return) {
    .base = {
      .vtable = &gltang_ast_node_return_vtable,
      .location = location,
      .is_singleton = false,
      .depth = depth,
    },
    .expression = expression,
  };
  return self;
}


void gltang_ast_node_return_destroy(GLTANG_Ast_Node * self) {
  assert(self);
  assert(GLTANG_AST_IS_RETURN(self));
  GLTANG_Ast_Node_Return * return_node = (GLTANG_Ast_Node_Return *)self;

  gltang_ast_node_destroy(return_node->expression);

  gcu_free(self);
}


void gltang_ast_node_return_print(GLTANG_Ast_Node * self, const char * indent) {
  assert(self);
  assert(GLTANG_AST_IS_RETURN(self));
  GLTANG_Ast_Node_Return * return_node = (GLTANG_Ast_Node_Return *)self;

  assert(indent);
  char * new_indent = gltang_ast_indent_extend(indent, 2);
  if (!new_indent) {
    return;
  }

  assert(return_node->expression);
  assert(self->vtable);
  assert(self->vtable->name);
  if (return_node->expression) {
    printf("%s%s:\n", indent, self->vtable->name);
    gltang_ast_node_print(return_node->expression, new_indent);
  }
  else {
    printf("%s%s\n", indent, self->vtable->name);
  }
  gltang_ast_indent_release(new_indent, indent);
}


void gltang_ast_node_return_walk(GLTANG_Ast_Node * self, GLTANG_Ast_Node_Walk_Callback callback, void * data, void * return_value) {
  assert(self);
  assert(GLTANG_AST_IS_RETURN(self));
  GLTANG_Ast_Node_Return * return_node = (GLTANG_Ast_Node_Return *)self;

  callback(self, data, return_value);

  assert(return_node->expression);
  gltang_ast_node_walk(return_node->expression, callback, data, return_value);
}

