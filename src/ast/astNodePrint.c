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
#include <ghoti.io/lang-tang/ast/astNodePrint.h>

GLTANG_Ast_Node_VTable gltang_ast_node_print_vtable = {
  .name = "Print",
  .destroy = gltang_ast_node_print_destroy,
  .print = gltang_ast_node_print_print,
  .walk = gltang_ast_node_print_walk,
};


GLTANG_Ast_Node_Print * gltang_ast_node_print_create(GLTANG_Ast_Node * expression, GLTANG_PARSER_LTYPE location) {
  assert(expression);

  GLTANG_Ast_Node_Print * self = gcu_malloc(sizeof(GLTANG_Ast_Node_Print));
  if (!self) {
    return 0;
  }

  *self = (GLTANG_Ast_Node_Print) {
    .base = {
      .vtable = &gltang_ast_node_print_vtable,
      .location = location,
      .is_singleton = false,
    },
    .expression = expression,
  };
  return self;
}


void gltang_ast_node_print_destroy(GLTANG_Ast_Node * self) {
  assert(self);
  assert(GLTANG_AST_IS_PRINT(self));
  GLTANG_Ast_Node_Print * print = (GLTANG_Ast_Node_Print *)self;

  gltang_ast_node_destroy(print->expression);
  gcu_free(self);
}


void gltang_ast_node_print_print(GLTANG_Ast_Node * self, const char * indent) {
  assert(self);
  assert(GLTANG_AST_IS_PRINT(self));
  GLTANG_Ast_Node_Print * print = (GLTANG_Ast_Node_Print *)self;

  assert(indent);
  size_t indent_len = strlen(indent);
  char * new_indent = gcu_malloc(indent_len + 3);
  if (!new_indent) {
    return;
  }
  memcpy(new_indent, indent, indent_len + 1);
  memcpy(new_indent + indent_len, "  ", 3);

  assert(self->vtable);
  assert(self->vtable->name);
  printf("%s%s():\n", indent, self->vtable->name);

  gltang_ast_node_print(print->expression, new_indent);
  gcu_free(new_indent);
}


void gltang_ast_node_print_walk(GLTANG_Ast_Node * self, GLTANG_Ast_Node_Walk_Callback callback, void * data, void * return_value) {
  assert(self);
  assert(GLTANG_AST_IS_PRINT(self));
  GLTANG_Ast_Node_Print * print = (GLTANG_Ast_Node_Print *)self;

  callback(self, data, return_value);
  callback(print->expression, data, return_value);
}

