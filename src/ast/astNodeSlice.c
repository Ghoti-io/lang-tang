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
#include <ghoti.io/lang-tang/ast/astNodeSlice.h>

GLTANG_Ast_Node_VTable gltang_ast_node_slice_vtable = {
  .name = "Slice",
  .destroy = gltang_ast_node_slice_destroy,
  .print = gltang_ast_node_slice_print,
  .walk = gltang_ast_node_slice_walk,
};


GLTANG_Ast_Node_Slice * gltang_ast_node_slice_create(GLTANG_Ast_Node * lhs, GLTANG_Ast_Node * start, GLTANG_Ast_Node * end, GLTANG_Ast_Node * skip, GLTANG_PARSER_LTYPE location) {
  assert(lhs);
  assert(start);
  assert(end);
  // Note: skip is optional.

  GLTANG_Ast_Node_Slice * self = gcu_malloc(sizeof(GLTANG_Ast_Node_Slice));
  if (!self) {
    return 0;
  }

  *self = (GLTANG_Ast_Node_Slice) {
    .base = {
      .vtable = &gltang_ast_node_slice_vtable,
      .location = location,
      .is_singleton = false,
    },
    .lhs = lhs,
    .start = start,
    .end = end,
    .skip = skip,
  };
  return self;
}


void gltang_ast_node_slice_destroy(GLTANG_Ast_Node * self) {
  assert(self);
  assert(GLTANG_AST_IS_SLICE(self));
  GLTANG_Ast_Node_Slice * slice = (GLTANG_Ast_Node_Slice *)self;

  GLTANG_Ast_Node * parts[] = {slice->lhs, slice->start, slice->end, slice->skip};
  for (size_t i = 0; i < 4; ++i) {
    if (parts[i]) {
      gltang_ast_node_destroy(parts[i]);
    }
  }
  gcu_free(self);
}


void gltang_ast_node_slice_print(GLTANG_Ast_Node * self, const char * indent) {
  assert(self);
  assert(GLTANG_AST_IS_SLICE(self));
  GLTANG_Ast_Node_Slice * slice = (GLTANG_Ast_Node_Slice *)self;

  assert(indent);
  size_t indent_len = strlen(indent);
  char * new_indent = gcu_malloc(indent_len + 5);
  if (!new_indent) {
    return;
  }
  memcpy(new_indent, indent, indent_len + 1);
  memcpy(new_indent + indent_len, "    ", 5);

  assert(self->vtable);
  assert(self->vtable->name);
  printf("%s%s:\n", indent, self->vtable->name);

  printf("%s  LHS:\n", indent);
  gltang_ast_node_print(slice->lhs, new_indent);

  printf("%s  Start:\n", indent);
  gltang_ast_node_print(slice->start, new_indent);

  printf("%s  End:\n", indent);
  gltang_ast_node_print(slice->end, new_indent);

  printf("%s  Skip:\n", indent);
  if (slice->skip) {
    gltang_ast_node_print(slice->skip, new_indent);
  }
  gcu_free(new_indent);
}


void gltang_ast_node_slice_walk(GLTANG_Ast_Node * self, GLTANG_Ast_Node_Walk_Callback callback, void * data, void * return_value) {
  assert(self);
  assert(GLTANG_AST_IS_SLICE(self));
  GLTANG_Ast_Node_Slice * slice = (GLTANG_Ast_Node_Slice *)self;

  callback(self, data, return_value);

  GLTANG_Ast_Node * parts[] = {slice->lhs, slice->start, slice->end, slice->skip};
  for (size_t i = 0; i < 4; ++i) {
    if (parts[i]) {
      gltang_ast_node_walk(parts[i], callback, data, return_value);
    }
  }
}

