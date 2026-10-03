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
#include <ghoti.io/lang-tang/ast/astNodeBoolean.h>

GLTANG_Ast_Node_VTable gltang_ast_node_boolean_vtable = {
  .name = "Boolean",
  .destroy = gltang_ast_node_boolean_destroy,
  .print = gltang_ast_node_boolean_print,
  .walk = gltang_ast_node_boolean_walk,
};


GLTANG_Ast_Node_Boolean * gltang_ast_node_boolean_create(bool value, GLTANG_PARSER_LTYPE location) {
  GLTANG_Ast_Node_Boolean * self = gcu_malloc(sizeof(GLTANG_Ast_Node_Boolean));
  if (!self) {
    return 0;
  }
  *self = (GLTANG_Ast_Node_Boolean) {
    .base = {
      .vtable = &gltang_ast_node_boolean_vtable,
      .location = location,
      .is_singleton = false,
      .depth = 1,
    },
    .value = value,
  };
  return self;
}


void gltang_ast_node_boolean_destroy(GLTANG_Ast_Node * self) {
  assert(self);
  gcu_free(self);
}


void gltang_ast_node_boolean_print(GLTANG_Ast_Node * self, const char * indent) {
  assert(self);
  assert(GLTANG_AST_IS_BOOLEAN(self));
  GLTANG_Ast_Node_Boolean * boolean = (GLTANG_Ast_Node_Boolean *) self;

  assert(indent);
  assert(self->vtable);
  assert(self->vtable->name);
  printf("%s%s: %s\n", indent, self->vtable->name, boolean->value ? "true" : "false");
}


void gltang_ast_node_boolean_walk(GLTANG_Ast_Node * self, GLTANG_Ast_Node_Walk_Callback callback, void * data, void * return_value) {
  assert(self);
  callback(self, data, return_value);
}

