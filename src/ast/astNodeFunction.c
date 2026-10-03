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
#include <ghoti.io/lang-tang/ast/astNodeFunction.h>
#include <ghoti.io/lang-tang/ast/astNodeIdentifier.h>
#include <ghoti.io/lang-tang/ast/astNodeParseError.h>

GLTANG_Ast_Node_VTable gltang_ast_node_function_vtable = {
  .name = "Function",
  .destroy = gltang_ast_node_function_destroy,
  .print = gltang_ast_node_function_print,
  .walk = gltang_ast_node_function_walk,
};


GLTANG_Ast_Node_Function * gltang_ast_node_function_create(const char * identifier, GLTANG_VectorX * parameters, GLTANG_Ast_Node * block, GLTANG_PARSER_LTYPE location) {
  assert(identifier);
  assert(parameters);
  assert(block);

  GLTANG_Ast_Node_Function * self = gcu_malloc(sizeof(GLTANG_Ast_Node_Function));
  if (!self) {
    return 0;
  }

  *self = (GLTANG_Ast_Node_Function) {
    .base = {
      .vtable = &gltang_ast_node_function_vtable,
      .location = location,
      .is_singleton = false,
    },
    .identifier = identifier,
    .hash = GLTANG_STRING_HASH(identifier, strlen(identifier)),
    .parameters = parameters,
    .block = block,
  };
  return self;
}


void gltang_ast_node_function_destroy(GLTANG_Ast_Node * self) {
  assert(self);
  assert(GLTANG_AST_IS_FUNCTION(self));
  GLTANG_Ast_Node_Function * function = (GLTANG_Ast_Node_Function *) self;

  GLTANG_VECTORX_DESTROY(function->parameters);
  gltang_ast_node_destroy(function->block);
  gcu_free((void *)function->identifier);
  gcu_free(self);
}


void gltang_ast_node_function_print(GLTANG_Ast_Node * self, const char * indent) {
  assert(self);
  assert(GLTANG_AST_IS_FUNCTION(self));
  GLTANG_Ast_Node_Function * function = (GLTANG_Ast_Node_Function *) self;

  assert(indent);
  char * new_indent = gcu_malloc(strlen(indent) + 5);
  if (!new_indent) {
    return;
  }
  size_t indent_len = strlen(indent);
  memcpy(new_indent, indent, indent_len + 1);
  memcpy(new_indent + indent_len, "    ", 5);

  char * small_indent = gcu_malloc(strlen(indent) + 3);
  if (!small_indent) {
    gcu_free(new_indent);
    return;
  }
  memcpy(small_indent, indent, indent_len + 1);
  memcpy(small_indent + indent_len, "  ", 3);

  assert(self->vtable);
  assert(self->vtable->name);
  printf("%s%s: %s\n", indent, self->vtable->name, function->identifier);
  printf("%s  Parameters:\n", indent);

  assert(function->parameters);
  assert(function->parameters->count ? (bool)function->parameters->data : true);
  for (size_t i = 0; i < GLTANG_VECTORX_COUNT(function->parameters); i++) {
    printf("%s%s\n", new_indent, ((GLTANG_Ast_Node_Identifier *)GLTANG_TYPEX_P(function->parameters->data[i]))->identifier);
  }

  assert(function->block);
  assert(function->block->vtable);
  assert(function->block->vtable->print);
  function->block->vtable->print(function->block, small_indent);

  gcu_free(new_indent);
  gcu_free(small_indent);
}


void gltang_ast_node_function_walk(GLTANG_Ast_Node * self, GLTANG_Ast_Node_Walk_Callback callback, void * data, void * return_value) {
  assert(self);
  assert(GLTANG_AST_IS_FUNCTION(self));
  GLTANG_Ast_Node_Function * function = (GLTANG_Ast_Node_Function *) self;

  callback(self, data, return_value);

  assert(function->parameters);
  assert(function->parameters->count ? (bool)function->parameters->data : true);
  for (size_t i = 0; i < GLTANG_VECTORX_COUNT(function->parameters); ++i) {
    gltang_ast_node_walk((GLTANG_Ast_Node *)GLTANG_TYPEX_P(function->parameters->data[i]), callback, data, return_value);
  }

  gltang_ast_node_walk(function->block, callback, data, return_value);
}

