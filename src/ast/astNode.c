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
#include <ghoti.io/lang-tang/ast/astNode.h>

GLTANG_Ast_Node_VTable gltang_ast_node_null_vtable = {
  .name = "Null",
  .destroy = gltang_ast_node_null_destroy,
  .print = gltang_ast_node_null_print,
  .walk = gltang_ast_node_null_walk,
};


GLTANG_Ast_Node * gltang_ast_node_create(GLTANG_PARSER_LTYPE location) {
  GLTANG_Ast_Node * self = gcu_malloc(sizeof(GLTANG_Ast_Node));
  if (!self) {
    return 0;
  }
  *self = (GLTANG_Ast_Node) {
    .vtable = &gltang_ast_node_null_vtable,
    .location = location,
    .is_singleton = false,
  };
  return self;
}


void gltang_ast_node_destroy(GLTANG_Ast_Node * self) {
  assert(self);
  assert(self->vtable);

  // A singleton has static storage, so there is nothing to give back.  The
  // is_singleton test used to be folded into the condition below, which sent
  // singletons to the fallback - and the fallback is a free(), not a no-op.
  // So the check that exists to protect a singleton was what freed it, and
  // every error reported by returning a parse-error singleton aborted the
  // process with "free(): invalid pointer" instead of failing compilation.
  if (self->is_singleton) {
    return;
  }

  self->vtable->destroy
    ? self->vtable->destroy(self)
    : gltang_ast_node_null_destroy(self);
}


void gltang_ast_node_print(GLTANG_Ast_Node * self, const char * indent) {
  assert(self);
  assert(self->vtable);
  self->vtable->print
    ? self->vtable->print(self, indent)
    : gltang_ast_node_null_print(self, indent);
}


void gltang_ast_node_walk(GLTANG_Ast_Node * self, GLTANG_Ast_Node_Walk_Callback callback, void * data, void * return_value) {
  assert(self);
  assert(self->vtable);
  self->vtable->walk
    ? self->vtable->walk(self, callback, data, return_value)
    : gltang_ast_node_null_walk(self, callback, data, return_value);
}


void gltang_ast_node_null_destroy(GLTANG_Ast_Node * self) {
  gcu_free(self);
}


void gltang_ast_node_null_print(GLTANG_Ast_Node * self, const char * indent) {
  assert(self);
  assert(indent);
  assert(self->vtable);
  printf("%s%s\n", indent, self->vtable->name);
}


void gltang_ast_node_null_walk(GLTANG_Ast_Node * self, GLTANG_Ast_Node_Walk_Callback callback, void * data, void * return_value) {
  callback(self, data, return_value);
}
