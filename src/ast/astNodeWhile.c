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
#include <ghoti.io/cutil/string.h>
#include <ghoti.io/lang-tang/macros.h>
#include <ghoti.io/lang-tang/ast/astNodeAssign.h>
#include <ghoti.io/lang-tang/ast/astNodeBinary.h>
#include <ghoti.io/lang-tang/ast/astNodeIdentifier.h>
#include <ghoti.io/lang-tang/ast/astNodeWhile.h>

GLTANG_Ast_Node_VTable gltang_ast_node_while_vtable = {
  .name = "While",
  .destroy = gltang_ast_node_while_destroy,
  .print = gltang_ast_node_while_print,
  .walk = gltang_ast_node_while_walk,
};


GLTANG_Ast_Node_While * gltang_ast_node_while_create(GLTANG_Ast_Node * condition, GLTANG_Ast_Node * block, GLTANG_PARSER_LTYPE location) {
  assert(condition);
  assert(block);

  GLTANG_Ast_Node_While * self = gcu_malloc(sizeof(GLTANG_Ast_Node_While));
  if (!self) {
    return 0;
  }

  *self = (GLTANG_Ast_Node_While) {
    .base = {
      .vtable = &gltang_ast_node_while_vtable,
      .location = location,
      .is_singleton = false,
    },
    .condition = condition,
    .block = block,
  };
  return self;
}


void gltang_ast_node_while_destroy(GLTANG_Ast_Node * self) {
  assert(self);
  assert(GLTANG_AST_IS_WHILE(self));
  GLTANG_Ast_Node_While * while_node = (GLTANG_Ast_Node_While *) self;

  gltang_ast_node_destroy(while_node->condition);
  gltang_ast_node_destroy(while_node->block);
  gcu_free(self);
}


void gltang_ast_node_while_print(GLTANG_Ast_Node * self, const char * indent) {
  assert(self);
  assert(GLTANG_AST_IS_WHILE(self));
  GLTANG_Ast_Node_While * while_node = (GLTANG_Ast_Node_While *) self;

  assert(indent);
  char * new_indent = gcu_malloc(strlen(indent) + 5);
  if (!new_indent) {
    return;
  }
  size_t indent_len = strlen(indent);
  memcpy(new_indent, indent, indent_len + 1);
  memcpy(new_indent + indent_len, "    ", 5);

  assert(self->vtable);
  assert(self->vtable->name);
  printf("%s%s\n", indent, self->vtable->name);

  printf("%s  Condition:\n", indent);
  gltang_ast_node_print(while_node->condition, new_indent);

  printf("%s  Block:\n", indent);
  gltang_ast_node_print(while_node->block, new_indent);
  gcu_free(new_indent);
}


void gltang_ast_node_while_walk(GLTANG_Ast_Node * self, GLTANG_Ast_Node_Walk_Callback callback, void * data, void * return_value) {
  assert(self);
  assert(GLTANG_AST_IS_WHILE(self));
  GLTANG_Ast_Node_While * while_node = (GLTANG_Ast_Node_While *) self;

  callback(self, data, return_value);

  gltang_ast_node_walk(while_node->condition, callback, data, return_value);
  gltang_ast_node_walk(while_node->block, callback, data, return_value);
}

