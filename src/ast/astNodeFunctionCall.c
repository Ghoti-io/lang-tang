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
#include <ghoti.io/lang-tang/ast/astNodeFunctionCall.h>

GLTANG_Ast_Node_VTable gltang_ast_node_function_call_vtable = {
  .name = "FunctionCall",
  .destroy = gltang_ast_node_function_call_destroy,
  .print = gltang_ast_node_function_call_print,
  .walk = gltang_ast_node_function_call_walk,
};


GLTANG_Ast_Node_Function_Call * gltang_ast_node_function_call_create(GLTANG_Ast_Node * lhs, GLTANG_VectorX * arguments, GLTANG_PARSER_LTYPE location) {
  assert(lhs);
  assert(arguments);

  GLTANG_Ast_Node_Function_Call * self = gcu_malloc(sizeof(GLTANG_Ast_Node_Function_Call));
  if (!self) {
    return 0;
  }

  *self = (GLTANG_Ast_Node_Function_Call) {
    .base = {
      .vtable = &gltang_ast_node_function_call_vtable,
      .location = location,
      .is_singleton = false,
    },
    .lhs = lhs,
    .arguments = arguments,
  };
  return self;
}


void gltang_ast_node_function_call_destroy(GLTANG_Ast_Node * self) {
  assert(self);
  assert(GLTANG_AST_IS_FUNCTION_CALL(self));
  GLTANG_Ast_Node_Function_Call * function_call = (GLTANG_Ast_Node_Function_Call *)self;

  gltang_ast_node_destroy(function_call->lhs);

  assert(function_call->arguments);
  GLTANG_VECTORX_DESTROY(function_call->arguments);
  gcu_free(self);
}


void gltang_ast_node_function_call_print(GLTANG_Ast_Node * self, const char * indent) {
  assert(self);
  assert(GLTANG_AST_IS_FUNCTION_CALL(self));
  GLTANG_Ast_Node_Function_Call * function_call = (GLTANG_Ast_Node_Function_Call *)self;

  assert(indent);
  char * new_indent = gcu_malloc(strlen(indent) + 5);
  if (!new_indent) {
    return;
  }
  size_t indent_len = strlen(indent);
  memcpy(new_indent, indent, indent_len + 1);
  memcpy(new_indent + indent_len, "    ", 5);

  assert(self->vtable);
  assert(self->vtable->name);
  printf("%s%s\n", indent, self->vtable->name);
  printf("%s  LHS:\n", indent);
  gltang_ast_node_print(function_call->lhs, new_indent);

  assert(function_call->arguments);
  assert(function_call->arguments->count ? (bool)function_call->arguments->data : true);
  printf("%s  Arguments:\n", indent);
  for (size_t i = 0; i < GLTANG_VECTORX_COUNT(function_call->arguments); i++) {
    printf("%s  %zu:\n", indent, i);
    gltang_ast_node_print((GLTANG_Ast_Node *)GLTANG_TYPEX_P(function_call->arguments->data[i]), new_indent);
  }
  gcu_free(new_indent);
}


void gltang_ast_node_function_call_walk(GLTANG_Ast_Node * self, GLTANG_Ast_Node_Walk_Callback callback, void * data, void * return_value) {
  assert(self);
  assert(GLTANG_AST_IS_FUNCTION_CALL(self));
  GLTANG_Ast_Node_Function_Call * function_call = (GLTANG_Ast_Node_Function_Call *)self;

  callback(self, data, return_value);
  gltang_ast_node_walk(function_call->lhs, callback, data, return_value);

  assert(function_call->arguments);
  assert(function_call->arguments->count ? (bool)function_call->arguments->data : true);
  for (size_t i = 0; i < GLTANG_VECTORX_COUNT(function_call->arguments); ++i) {
    GLTANG_Ast_Node * argument = (GLTANG_Ast_Node *)GLTANG_TYPEX_P(function_call->arguments->data[i]);
    gltang_ast_node_walk(argument, callback, data, return_value);
  }
}

