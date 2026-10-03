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
#include <ghoti.io/lang-tang/ast/astNodeIndex.h>

GLTANG_Ast_Node_VTable gltang_ast_node_index_vtable = {
  .name = "Index",
  .destroy = gltang_ast_node_index_destroy,
  .print = gltang_ast_node_index_print,
  .walk = gltang_ast_node_index_walk,
};


GLTANG_Ast_Node_Index * gltang_ast_node_index_create(GLTANG_Ast_Node * lhs, GLTANG_Ast_Node * rhs, GLTANG_PARSER_LTYPE location) {
  assert(lhs);
  assert(rhs);

  uint32_t depth = 1 + gltang_ast_height_max(gltang_ast_height(lhs), gltang_ast_height(rhs));

  if (!gltang_ast_depth_ok(depth)) {

    return 0;

  }


  GLTANG_Ast_Node_Index * self = gcu_malloc(sizeof(GLTANG_Ast_Node_Index));
  if (!self) {
    return 0;
  }

  *self = (GLTANG_Ast_Node_Index) {
    .base = {
      .vtable = &gltang_ast_node_index_vtable,
      .location = location,
      .is_singleton = false,
      .depth = depth,
    },
    .lhs = lhs,
    .rhs = rhs,
  };
  return self;
}


void gltang_ast_node_index_destroy(GLTANG_Ast_Node * self) {
  assert(self);
  assert(GLTANG_AST_IS_INDEX(self));
  GLTANG_Ast_Node_Index * index = (GLTANG_Ast_Node_Index *) self;

  gltang_ast_node_destroy(index->lhs);
  gltang_ast_node_destroy(index->rhs);
  gcu_free(self);
}


void gltang_ast_node_index_print(GLTANG_Ast_Node * self, const char * indent) {
  assert(self);
  assert(GLTANG_AST_IS_INDEX(self));
  GLTANG_Ast_Node_Index * index = (GLTANG_Ast_Node_Index *) self;

  assert(indent);
  char * new_indent = gltang_ast_indent_extend(indent, 4);
  if (!new_indent) {
    return;
  }

  assert(self->vtable);
  assert(self->vtable->name);
  printf("%s%s\n", indent, self->vtable->name);

  printf("%s  LHS:\n", indent);
  gltang_ast_node_print(index->lhs, new_indent);

  printf("%s  RHS:\n", indent);
  gltang_ast_node_print(index->rhs, new_indent);
  gltang_ast_indent_release(new_indent, indent);
}


void gltang_ast_node_index_walk(GLTANG_Ast_Node * self, GLTANG_Ast_Node_Walk_Callback callback, void * data, void * return_value) {
  assert(self);
  assert(GLTANG_AST_IS_INDEX(self));
  GLTANG_Ast_Node_Index * index = (GLTANG_Ast_Node_Index *) self;

  callback(self, data, return_value);

  gltang_ast_node_walk(index->lhs, callback, data, return_value);
  gltang_ast_node_walk(index->rhs, callback, data, return_value);
}

