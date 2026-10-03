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
#include <ghoti.io/lang-tang/ast/astNodeArray.h>

GLTANG_Ast_Node_VTable gltang_ast_node_array_vtable = {
  .name = "Array",
  .destroy = gltang_ast_node_array_destroy,
  .print = gltang_ast_node_array_print,
  .walk = gltang_ast_node_array_walk,
};


GLTANG_Ast_Node_Array * gltang_ast_node_array_create(GLTANG_VectorX * elements, GLTANG_PARSER_LTYPE location) {
  assert(elements);
  uint32_t depth = 1 + gltang_ast_height_of_nodes(elements);
  if (!gltang_ast_depth_ok(depth)) {
    return 0;
  }

  GLTANG_Ast_Node_Array * self = gcu_malloc(sizeof(GLTANG_Ast_Node_Array));
  if (!self) {
    return 0;
  }
  *self = (GLTANG_Ast_Node_Array) {
    .base = {
      .vtable = &gltang_ast_node_array_vtable,
      .location = location,
      .is_singleton = false,
      .depth = depth,
    },
    .elements = elements,
  };
  return self;
}


void gltang_ast_node_array_destroy(GLTANG_Ast_Node * self) {
  assert(self);
  assert(GLTANG_AST_IS_ARRAY(self));
  GLTANG_Ast_Node_Array * array = (GLTANG_Ast_Node_Array *) self;
  GLTANG_VECTORX_DESTROY(array->elements);
  gcu_free(self);
}


void gltang_ast_node_array_print(GLTANG_Ast_Node * self, const char * indent) {
  assert(self);
  assert(GLTANG_AST_IS_ARRAY(self));
  GLTANG_Ast_Node_Array * array = (GLTANG_Ast_Node_Array *) self;

  assert(indent);
  char * new_indent = gltang_ast_indent_extend(indent, 2);
  if (!new_indent) {
    return;
  }
  printf("%s%s\n", indent, self->vtable->name);

  assert(array->elements);
  assert(array->elements->count ? (bool)array->elements->data : true);
  for (size_t i = 0; i < GLTANG_VECTORX_COUNT(array->elements); i++) {
    gltang_ast_node_print((GLTANG_Ast_Node *) array->elements->data[i].p, new_indent);
  }
  gltang_ast_indent_release(new_indent, indent);
}


void gltang_ast_node_array_walk(GLTANG_Ast_Node * self, GLTANG_Ast_Node_Walk_Callback callback, void * data, void * return_value) {
  assert(self);
  assert(GLTANG_AST_IS_ARRAY(self));
  GLTANG_Ast_Node_Array * array = (GLTANG_Ast_Node_Array *) self;

  callback(self, data, return_value);

  assert(array->elements);
  assert(array->elements->count ? (bool)array->elements->data : true);
  for (size_t i = 0; i < GLTANG_VECTORX_COUNT(array->elements); ++i) {
    GLTANG_Ast_Node * element = (GLTANG_Ast_Node *) array->elements->data[i].p;
    gltang_ast_node_walk(element, callback, data, return_value);
  }
}

