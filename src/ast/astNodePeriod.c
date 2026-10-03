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
#include <ghoti.io/cutil/string.h>
#include <ghoti.io/lang-tang/macros.h>
#include <ghoti.io/lang-tang/ast/astNodePeriod.h>

GLTANG_Ast_Node_VTable gltang_ast_node_period_vtable = {
  .name = "Period",
  .destroy = gltang_ast_node_period_destroy,
  .print = gltang_ast_node_period_print,
  .walk = gltang_ast_node_period_walk,
};


GLTANG_Ast_Node_Period * gltang_ast_node_period_create(GLTANG_Ast_Node * lhs, const char * rhs, GLTANG_PARSER_LTYPE location) {
  assert(lhs);
  assert(rhs);

  GLTANG_Ast_Node_Period * self = gcu_malloc(sizeof(GLTANG_Ast_Node_Period));
  if (!self) {
    return 0;
  }

  *self = (GLTANG_Ast_Node_Period) {
    .base = {
      .vtable = &gltang_ast_node_period_vtable,
      .location = location,
      .is_singleton = false,
    },
    .lhs = lhs,
    .rhs = rhs,
  };
  return self;
}


void gltang_ast_node_period_destroy(GLTANG_Ast_Node * self) {
  assert(self);
  assert(GLTANG_AST_IS_PERIOD(self));
  GLTANG_Ast_Node_Period * period = (GLTANG_Ast_Node_Period *) self;

  gltang_ast_node_destroy(period->lhs);
  gcu_free((void *)period->rhs);
  gcu_free(self);
}


void gltang_ast_node_period_print(GLTANG_Ast_Node * self, const char * indent) {
  assert(self);
  assert(GLTANG_AST_IS_PERIOD(self));
  GLTANG_Ast_Node_Period * period = (GLTANG_Ast_Node_Period *) self;

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
  gltang_ast_node_print(period->lhs, new_indent);

  printf("%s  RHS: %s\n", indent, period->rhs);
  gcu_free(new_indent);
}


void gltang_ast_node_period_walk(GLTANG_Ast_Node * self, GLTANG_Ast_Node_Walk_Callback callback, void * data, void * return_value) {
  assert(self);
  assert(GLTANG_AST_IS_PERIOD(self));
  GLTANG_Ast_Node_Period * period = (GLTANG_Ast_Node_Period *) self;

  callback(self, data, return_value);
  gltang_ast_node_walk(period->lhs, callback, data, return_value);
}

