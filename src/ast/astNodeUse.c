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
#include <ghoti.io/cutil/hash.h>
#include <ghoti.io/lang-tang/macros.h>
#include <ghoti.io/lang-tang/ast/astNodeParseError.h>
#include <ghoti.io/lang-tang/ast/astNodeUse.h>


GLTANG_Ast_Node_VTable gltang_ast_node_use_vtable = {
  .name = "Use",
  .destroy = gltang_ast_node_use_destroy,
  .print = gltang_ast_node_use_print,
  .walk = gltang_ast_node_use_walk,
};


GLTANG_Ast_Node_Use * gltang_ast_node_use_create(const char * identifier, GLTANG_Ast_Node * expression, GLTANG_PARSER_LTYPE location) {
  assert(identifier);
  // Note: expression can be NULL.

  GLTANG_Ast_Node_Use * self = gcu_malloc(sizeof(GLTANG_Ast_Node_Use));
  if (!self) {
    return 0;
  }

  *self = (GLTANG_Ast_Node_Use) {
    .base = {
      .vtable = &gltang_ast_node_use_vtable,
      .location = location,
      .is_singleton = false,
    },
    .identifier = identifier,
    .hash = GLTANG_STRING_HASH(identifier, strlen(identifier)),
    .expression = expression,
  };
  return self;
}


void gltang_ast_node_use_destroy(GLTANG_Ast_Node * self) {
  assert(self);
  assert(GLTANG_AST_IS_USE(self));
  GLTANG_Ast_Node_Use * use = (GLTANG_Ast_Node_Use *)self;

  gcu_free((void *)use->identifier);
  if (use->expression) {
    gltang_ast_node_destroy(use->expression);
  }
  gcu_free(self);
}


void gltang_ast_node_use_print(GLTANG_Ast_Node * self, const char * indent) {
  assert(self);
  assert(GLTANG_AST_IS_USE(self));
  GLTANG_Ast_Node_Use * use = (GLTANG_Ast_Node_Use *)self;

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
  printf("%s%s(%s):\n", indent, self->vtable->name, use->identifier);
  if (use->expression) {
    gltang_ast_node_print(use->expression, new_indent);
  }
  gcu_free(new_indent);
}


void gltang_ast_node_use_walk(GLTANG_Ast_Node * self, GLTANG_Ast_Node_Walk_Callback callback, void * data, void * return_value) {
  assert(self);
  assert(GLTANG_AST_IS_USE(self));
  GLTANG_Ast_Node_Use * use = (GLTANG_Ast_Node_Use *)self;

  callback(self, data, return_value);
  gltang_ast_node_walk(use->expression, callback, data, return_value);
}

