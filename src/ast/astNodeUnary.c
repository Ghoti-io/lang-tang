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
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <ghoti.io/cutil/memory.h>
#include <ghoti.io/lang-tang/macros.h>
#include <ghoti.io/lang-tang/parse.h>
#include "ast_internal.h"
#include <ghoti.io/lang-tang/ast/astNodeBoolean.h>
#include <ghoti.io/lang-tang/ast/astNodeFloat.h>
#include <ghoti.io/lang-tang/ast/astNodeInteger.h>
#include <ghoti.io/lang-tang/ast/astNodeString.h>
#include <ghoti.io/lang-tang/ast/astNodeUnary.h>

GLTANG_Ast_Node_VTable gltang_ast_node_unary_vtable = {
  .name = "Unary",
  .destroy = gltang_ast_node_unary_destroy,
  .print = gltang_ast_node_unary_print,
  .walk = gltang_ast_node_unary_walk,
};


GLTANG_Ast_Node_Unary * gltang_ast_node_unary_create(GLTANG_Ast_Node * expression, GLTANG_Unary_Type operator_type, GLTANG_PARSER_LTYPE location) {
  assert(expression);

  uint32_t depth = 1 + gltang_ast_height(expression);

  if (!gltang_ast_depth_ok(depth)) {

    return 0;

  }


  GLTANG_Ast_Node_Unary * self = gcu_malloc(sizeof(GLTANG_Ast_Node_Unary));
  if (!self) {
    return 0;
  }

  *self = (GLTANG_Ast_Node_Unary) {
    .base = {
      .vtable = &gltang_ast_node_unary_vtable,
      .location = location,
      .is_singleton = false,
      .depth = depth,
    },
    .expression = expression,
    .operator_type = operator_type,
  };
  return self;
}


void gltang_ast_node_unary_destroy(GLTANG_Ast_Node * self) {
  assert(self);
  assert(GLTANG_AST_IS_UNARY(self));
  GLTANG_Ast_Node_Unary * unary = (GLTANG_Ast_Node_Unary *) self;

  gltang_ast_node_destroy(unary->expression);
  gcu_free(self);
}


void gltang_ast_node_unary_print(GLTANG_Ast_Node * self, const char * indent) {
  assert(self);
  assert(GLTANG_AST_IS_UNARY(self));
  GLTANG_Ast_Node_Unary * unary = (GLTANG_Ast_Node_Unary *) self;

  assert(indent);
  char * new_indent = gltang_ast_indent_extend(indent, 2);
  if (!new_indent) {
    return;
  }

  assert(self->vtable);
  assert(self->vtable->name);
  printf("%s%s(%s):\n", indent, self->vtable->name,
    unary->operator_type == GLTANG_UNARY_TYPE_NEGATIVE
      ? "-"
      : unary->operator_type == GLTANG_UNARY_TYPE_NOT
        ? "!"
        : "unknown");
  gltang_ast_node_print(unary->expression, new_indent);
  gltang_ast_indent_release(new_indent, indent);
}


void gltang_ast_node_unary_walk(GLTANG_Ast_Node * self, GLTANG_Ast_Node_Walk_Callback callback, void * data, void * return_value) {
  assert(self);
  assert(GLTANG_AST_IS_UNARY(self));
  GLTANG_Ast_Node_Unary * unary = (GLTANG_Ast_Node_Unary *) self;

  callback(self, data, return_value);

  gltang_ast_node_walk(unary->expression, callback, data, return_value);
}
