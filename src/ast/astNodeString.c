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
#include <ghoti.io/lang-tang/ast/astNodeString.h>

GLTANG_Ast_Node_VTable gltang_ast_node_string_vtable = {
  .name = "String",
  .destroy = gltang_ast_node_string_destroy,
  .print = gltang_ast_node_string_print,
  .walk = gltang_ast_node_string_walk,
};


GLTANG_Ast_Node_String * gltang_ast_node_string_create(GLTANG_Unicode_String * string, GLTANG_PARSER_LTYPE location) {
  assert(string);

  GLTANG_Ast_Node_String * self = gcu_malloc(sizeof(GLTANG_Ast_Node_String));
  if (!self) {
    return 0;
  }

  *self = (GLTANG_Ast_Node_String) {
    .base = {
      .vtable = &gltang_ast_node_string_vtable,
      .location = location,
      .is_singleton = false,
    },
    .string = string,
  };
  return self;
}


void gltang_ast_node_string_destroy(GLTANG_Ast_Node * self) {
  assert(self);
  assert(GLTANG_AST_IS_STRING(self));
  GLTANG_Ast_Node_String * string = (GLTANG_Ast_Node_String *)self;

  gltang_unicode_string_destroy(string->string);
  gcu_free(self);
}


void gltang_ast_node_string_print(GLTANG_Ast_Node * self, const char * indent) {
  assert(self);
  assert(GLTANG_AST_IS_STRING(self));
  GLTANG_Ast_Node_String * string = (GLTANG_Ast_Node_String *)self;

  assert(indent);
  assert(self->vtable);
  assert(self->vtable->name);
  assert(string->string);
  assert(string->string->buffer);
  printf("%s%s: \"%s\"\n", indent, self->vtable->name, string->string->buffer);
}


void gltang_ast_node_string_walk(GLTANG_Ast_Node * self, GLTANG_Ast_Node_Walk_Callback callback, void * data, void * return_value) {
  assert(self);
  callback(self, data, return_value);
}

