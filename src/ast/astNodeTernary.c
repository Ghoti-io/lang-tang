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
#include <ghoti.io/lang-tang/ast/astNodeTernary.h>

GLTANG_Ast_Node_VTable gltang_ast_node_ternary_vtable = {
  .name = "Ternary",
  .destroy = gltang_ast_node_ternary_destroy,
  .print = gltang_ast_node_ternary_print,
  .walk = gltang_ast_node_ternary_walk,
};


GLTANG_Ast_Node_Ternary * gltang_ast_node_ternary_create(GLTANG_Ast_Node * condition, GLTANG_Ast_Node * ifTrue, GLTANG_Ast_Node * ifFalse, GLTANG_PARSER_LTYPE location) {
  assert(condition);
  assert(ifTrue);
  assert(ifFalse);

  uint32_t depth = 1 + gltang_ast_height_max(gltang_ast_height_max(gltang_ast_height(condition), gltang_ast_height(ifTrue)), gltang_ast_height(ifFalse));

  if (!gltang_ast_depth_ok(depth)) {

    return 0;

  }


  GLTANG_Ast_Node_Ternary * self = gcu_malloc(sizeof(GLTANG_Ast_Node_Ternary));
  if (!self) {
    return 0;
  }

  *self = (GLTANG_Ast_Node_Ternary) {
    .base = {
      .vtable = &gltang_ast_node_ternary_vtable,
      .location = location,
      .is_singleton = false,
      .depth = depth,
    },
    .condition = condition,
    .ifTrue = ifTrue,
    .ifFalse = ifFalse,
  };
  return self;
}


void gltang_ast_node_ternary_destroy(GLTANG_Ast_Node * self) {
  assert(self);
  assert(GLTANG_AST_IS_TERNARY(self));
  GLTANG_Ast_Node_Ternary * ternary = (GLTANG_Ast_Node_Ternary *)self;

  gltang_ast_node_destroy(ternary->condition);
  gltang_ast_node_destroy(ternary->ifTrue);
  gltang_ast_node_destroy(ternary->ifFalse);
  gcu_free(self);
}


void gltang_ast_node_ternary_print(GLTANG_Ast_Node * self, const char * indent) {
  assert(self);
  assert(GLTANG_AST_IS_TERNARY(self));
  GLTANG_Ast_Node_Ternary * ternary = (GLTANG_Ast_Node_Ternary *)self;

  assert(indent);
  char * new_indent = gltang_ast_indent_extend(indent, 4);
  if (!new_indent) {
    return;
  }

  assert(self->vtable);
  assert(self->vtable->name);
  printf("%s%s:\n", indent, self->vtable->name);

  printf("%s  Condition:\n", indent);
  gltang_ast_node_print(ternary->condition, new_indent);

  printf("%s  If True:\n", indent);
  gltang_ast_node_print(ternary->ifTrue, new_indent);

  printf("%s  If False:\n", indent);
  gltang_ast_node_print(ternary->ifFalse, new_indent);
  gltang_ast_indent_release(new_indent, indent);
}


void gltang_ast_node_ternary_walk(GLTANG_Ast_Node * self, GLTANG_Ast_Node_Walk_Callback callback, void * data, void * return_value) {
  assert(self);
  assert(GLTANG_AST_IS_TERNARY(self));
  GLTANG_Ast_Node_Ternary * ternary = (GLTANG_Ast_Node_Ternary *)self;

  callback(self, data, return_value);

  gltang_ast_node_walk(ternary->condition, callback, data, return_value);
  gltang_ast_node_walk(ternary->ifTrue, callback, data, return_value);
  gltang_ast_node_walk(ternary->ifFalse, callback, data, return_value);
}

