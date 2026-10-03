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
#include <ghoti.io/lang-tang/ast/astNodeContinue.h>

GLTANG_Ast_Node_VTable gltang_ast_node_continue_vtable = {
  .name = "Continue",
  .destroy = gltang_ast_node_continue_destroy,
  .print = gltang_ast_node_continue_print,
  .walk = gltang_ast_node_continue_walk,
};


GLTANG_Ast_Node_Continue * gltang_ast_node_continue_create(GLTANG_PARSER_LTYPE location) {
  GLTANG_Ast_Node_Continue * self = gcu_malloc(sizeof(GLTANG_Ast_Node_Continue));
  if (!self) {
    return 0;
  }
  *self = (GLTANG_Ast_Node_Continue) {
    .base = {
      .vtable = &gltang_ast_node_continue_vtable,
      .location = location,
      .is_singleton = false,
    },
  };
  return self;
}


void gltang_ast_node_continue_destroy(GLTANG_Ast_Node * self) {
  assert(self);
  gcu_free(self);
}


void gltang_ast_node_continue_print(GLTANG_Ast_Node * self, const char * indent) {
  assert(self);
  assert(GLTANG_AST_IS_CONTINUE(self));
  assert(indent);
  assert(self->vtable);
  assert(self->vtable->name);
  printf("%s%s\n", indent, self->vtable->name);
}


void gltang_ast_node_continue_walk(GLTANG_Ast_Node * self, GLTANG_Ast_Node_Walk_Callback callback, void * data, void * return_value) {
  assert(self);
  callback(self, data, return_value);
}

