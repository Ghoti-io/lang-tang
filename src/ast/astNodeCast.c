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
#include <ghoti.io/lang-tang/ast/astNodeBoolean.h>
#include <ghoti.io/lang-tang/ast/astNodeCast.h>
#include <ghoti.io/lang-tang/ast/astNodeFloat.h>
#include <ghoti.io/lang-tang/ast/astNodeInteger.h>
#include <ghoti.io/lang-tang/ast/astNodeString.h>

GLTANG_Ast_Node_VTable gltang_ast_node_cast_vtable = {
  .name = "Cast",
  .destroy = gltang_ast_node_cast_destroy,
  .print = gltang_ast_node_cast_print,
  .walk = gltang_ast_node_cast_walk,
};


GLTANG_Ast_Node_Cast * gltang_ast_node_cast_create(GLTANG_Ast_Node * expression, GLTANG_Cast_Type type, GLTANG_PARSER_LTYPE location) {
  assert(expression);

  GLTANG_Ast_Node_Cast * self = gcu_malloc(sizeof(GLTANG_Ast_Node_Cast));
  if (!self) {
    return 0;
  }
  *self = (GLTANG_Ast_Node_Cast) {
    .base = {
      .vtable = &gltang_ast_node_cast_vtable,
      .location = location,
      .is_singleton = false,
    },
    .expression = expression,
    .type = type,
  };
  return self;
}


void gltang_ast_node_cast_destroy(GLTANG_Ast_Node * self) {
  assert(self);
  assert(GLTANG_AST_IS_CAST(self));
  GLTANG_Ast_Node_Cast * cast = (GLTANG_Ast_Node_Cast *) self;

  if (cast->expression) {
    gltang_ast_node_destroy(cast->expression);
  }
  gcu_free(self);
}


void gltang_ast_node_cast_print(GLTANG_Ast_Node * self, const char * indent) {
  assert(self);
  assert(GLTANG_AST_IS_CAST(self));
  GLTANG_Ast_Node_Cast * cast = (GLTANG_Ast_Node_Cast *) self;

  assert(indent);
  assert(self->vtable);
  assert(self->vtable->name);
  printf("%s%s as %s:\n", indent, self->vtable->name,
    cast->type == GLTANG_CAST_TYPE_INTEGER
      ? "integer"
      : cast->type == GLTANG_CAST_TYPE_FLOAT
        ? "float"
        : cast->type == GLTANG_CAST_TYPE_BOOLEAN
          ? "boolean"
          : cast->type == GLTANG_CAST_TYPE_STRING
            ? "string"
            : "unknown");

  size_t indent_length = strlen(indent);
  char * new_indent = gcu_malloc(indent_length + 3);
  if (!new_indent) {
    return;
  }
  memcpy(new_indent, indent, indent_length + 1);
  memcpy(new_indent + indent_length, "  ", 3);
  gltang_ast_node_print(cast->expression, new_indent);
  gcu_free(new_indent);
}


void gltang_ast_node_cast_walk(GLTANG_Ast_Node * self, GLTANG_Ast_Node_Walk_Callback callback, void * data, void * return_value) {
  assert(self);
  assert(GLTANG_AST_IS_CAST(self));
  GLTANG_Ast_Node_Cast * cast = (GLTANG_Ast_Node_Cast *) self;

  callback(self, data, return_value);
  callback(cast->expression, data, return_value);
}

