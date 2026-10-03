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
#include <ghoti.io/lang-tang/ast/astNodeBinary.h>
#include <ghoti.io/lang-tang/ast/astNodeInteger.h>
#include <ghoti.io/lang-tang/ast/astNodeFloat.h>
#include <ghoti.io/lang-tang/ast/astNodeBoolean.h>
#include <ghoti.io/lang-tang/ast/astNodeString.h>
#include <ghoti.io/lang-tang/unicodeString.h>

GLTANG_Ast_Node_VTable gltang_ast_node_binary_vtable = {
  .name = "Binary",
  .destroy = gltang_ast_node_binary_destroy,
  .print = gltang_ast_node_binary_print,
  .walk = gltang_ast_node_binary_walk,
};


GLTANG_Ast_Node_Binary * gltang_ast_node_binary_create(GLTANG_Ast_Node * lhs, GLTANG_Ast_Node * rhs, GLTANG_Binary_Type operator_type, GLTANG_PARSER_LTYPE location) {
  assert(lhs);
  assert(rhs);

  GLTANG_Ast_Node_Binary * self = gcu_malloc(sizeof(GLTANG_Ast_Node_Binary));
  if (!self) {
    return 0;
  }

  *self = (GLTANG_Ast_Node_Binary) {
    .base = {
      .vtable = &gltang_ast_node_binary_vtable,
      .location = location,
      .is_singleton = false,
    },
    .lhs = lhs,
    .rhs = rhs,
    .operator_type = operator_type,
  };
  return self;
}


void gltang_ast_node_binary_destroy(GLTANG_Ast_Node * self) {
  assert(self);
  assert(GLTANG_AST_IS_BINARY(self));
  GLTANG_Ast_Node_Binary * binary = (GLTANG_Ast_Node_Binary *) self;

  gltang_ast_node_destroy(binary->lhs);
  gltang_ast_node_destroy(binary->rhs);

  gcu_free(self);
}


void gltang_ast_node_binary_print(GLTANG_Ast_Node * self, const char * indent) {
  assert(self);
  assert(GLTANG_AST_IS_BINARY(self));
  GLTANG_Ast_Node_Binary * binary = (GLTANG_Ast_Node_Binary *) self;

  assert(indent);
  char * new_indent = gcu_malloc(strlen(indent) + 5);
  if (!new_indent) {
    return;
  }

  size_t indent_len = strlen(indent);
  memcpy(new_indent, indent, indent_len + 1);
  memcpy(new_indent + indent_len, "    ", 5);

  const char * operator_str = 0;
  switch(binary->operator_type) {
    case GLTANG_BINARY_TYPE_ADD:
      operator_str = "+";
      break;
    case GLTANG_BINARY_TYPE_SUBTRACT:
      operator_str = "-";
      break;
    case GLTANG_BINARY_TYPE_MULTIPLY:
      operator_str = "*";
      break;
    case GLTANG_BINARY_TYPE_DIVIDE:
      operator_str = "/";
      break;
    case GLTANG_BINARY_TYPE_MODULO:
      operator_str = "%";
      break;
    case GLTANG_BINARY_TYPE_LESS_THAN:
      operator_str = "<";
      break;
    case GLTANG_BINARY_TYPE_LESS_THAN_EQUAL:
      operator_str = "<=";
      break;
    case GLTANG_BINARY_TYPE_GREATER_THAN:
      operator_str = ">";
      break;
    case GLTANG_BINARY_TYPE_GREATER_THAN_EQUAL:
      operator_str = ">=";
      break;
    case GLTANG_BINARY_TYPE_EQUAL:
      operator_str = "==";
      break;
    case GLTANG_BINARY_TYPE_NOT_EQUAL:
      operator_str = "!=";
      break;
    case GLTANG_BINARY_TYPE_AND:
      operator_str = "&&";
      break;
    case GLTANG_BINARY_TYPE_OR:
      operator_str = "||";
      break;
  }

  assert(self->vtable);
  assert(self->vtable->name);
  printf("%s%s (%s):\n", indent, self->vtable->name, operator_str);

  printf("%s  LHS:\n", indent);
  gltang_ast_node_print(binary->lhs, new_indent);

  printf("%s  RHS:\n", indent);
  gltang_ast_node_print(binary->rhs, new_indent);

  gcu_free(new_indent);
}


void gltang_ast_node_binary_walk(GLTANG_Ast_Node * self, GLTANG_Ast_Node_Walk_Callback callback, void * data, void * return_value) {
  assert(self);
  assert(GLTANG_AST_IS_BINARY(self));
  GLTANG_Ast_Node_Binary * binary = (GLTANG_Ast_Node_Binary *) self;

  callback(self, data, return_value);

  gltang_ast_node_walk(binary->lhs, callback, data, return_value);
  gltang_ast_node_walk(binary->rhs, callback, data, return_value);
}

