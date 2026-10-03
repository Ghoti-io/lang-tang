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
#include <ghoti.io/lang-tang/ast/astNodeLibrary.h>

GLTANG_Ast_Node_VTable gltang_ast_node_library_vtable = {
  .name = "Library",
  .destroy = gltang_ast_node_library_destroy,
  .print = gltang_ast_node_library_print,
  .walk = gltang_ast_node_library_walk,
};


GLTANG_Ast_Node_Library * gltang_ast_node_library_create(const char * identifier, GLTANG_PARSER_LTYPE location) {
  assert(identifier);

  GLTANG_Ast_Node_Library * self = gcu_malloc(sizeof(GLTANG_Ast_Node_Library));
  if (!self) {
    return 0;
  }

  *self = (GLTANG_Ast_Node_Library) {
    .base = {
      .vtable = &gltang_ast_node_library_vtable,
      .location = location,
      .is_singleton = false,
    },
    .identifier = identifier,
    .hash = GLTANG_STRING_HASH(identifier, strlen(identifier)),
  };
  return self;
}


void gltang_ast_node_library_destroy(GLTANG_Ast_Node * self) {
  assert(self);
  assert(GLTANG_AST_IS_LIBRARY(self));
  GLTANG_Ast_Node_Library * library = (GLTANG_Ast_Node_Library *) self;

  gcu_free((void *)library->identifier);
  gcu_free(self);
}


void gltang_ast_node_library_print(GLTANG_Ast_Node * self, const char * indent) {
  assert(self);
  assert(GLTANG_AST_IS_LIBRARY(self));
  GLTANG_Ast_Node_Library * library = (GLTANG_Ast_Node_Library *) self;

  assert(indent);
  assert(self->vtable);
  assert(self->vtable->name);
  printf("%s%s: %s\n", indent, self->vtable->name, library->identifier);
}


void gltang_ast_node_library_walk(GLTANG_Ast_Node * self, GLTANG_Ast_Node_Walk_Callback callback, void * data, void * return_value) {
  assert(self);
  callback(self, data, return_value);
}

