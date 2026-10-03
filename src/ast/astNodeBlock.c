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
#include <ghoti.io/lang-tang/ast/astNodeBlock.h>
#include <ghoti.io/lang-tang/ast/astNodeUse.h>
#include <ghoti.io/lang-tang/ast/astNodeFunction.h>
#include <ghoti.io/lang-tang/ast/astNodeGlobal.h>

GLTANG_Ast_Node_VTable gltang_ast_node_block_vtable = {
  .name = "Block",
  .destroy = gltang_ast_node_block_destroy,
  .print = gltang_ast_node_block_print,
  .walk = gltang_ast_node_block_walk,
};


GLTANG_Ast_Node_Block * gltang_ast_node_block_create(GLTANG_VectorX * statements, GLTANG_PARSER_LTYPE location) {
  assert(statements);

  GLTANG_Ast_Node_Block * self = gcu_malloc(sizeof(GLTANG_Ast_Node_Block));
  if (!self) {
    return 0;
  }

  *self = (GLTANG_Ast_Node_Block) {
    .base = {
      .vtable = &gltang_ast_node_block_vtable,
      .location = location,
      .is_singleton = false,
    },
    .statements = statements,
  };
  return self;
}


void gltang_ast_node_block_destroy(GLTANG_Ast_Node * self) {
  assert(self);
  assert(GLTANG_AST_IS_BLOCK(self));
  GLTANG_Ast_Node_Block * block = (GLTANG_Ast_Node_Block *) self;

  GLTANG_VECTORX_DESTROY(block->statements);
  gcu_free(self);
}


void gltang_ast_node_block_print(GLTANG_Ast_Node * self, const char * indent) {
  assert(self);
  assert(GLTANG_AST_IS_BLOCK(self));
  GLTANG_Ast_Node_Block * block = (GLTANG_Ast_Node_Block *) self;

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
  printf("%s%s:\n", indent, self->vtable->name);

  assert(block->statements);
  assert(block->statements->count ? (bool)block->statements->data : true);
  for (size_t i = 0; i < GLTANG_VECTORX_COUNT(block->statements); ++i) {
    GLTANG_Ast_Node * statement = (GLTANG_Ast_Node *)GLTANG_TYPEX_P(block->statements->data[i]);
    gltang_ast_node_print(statement, new_indent);
  }
  gcu_free(new_indent);
}


void gltang_ast_node_block_walk(GLTANG_Ast_Node * self, GLTANG_Ast_Node_Walk_Callback callback, void * data, void * return_value) {
  assert(self);
  assert(GLTANG_AST_IS_BLOCK(self));
  GLTANG_Ast_Node_Block * block = (GLTANG_Ast_Node_Block *) self;

  callback(self, data, return_value);

  assert(block->statements);
  assert(block->statements->count ? (bool)block->statements->data : true);
  for (size_t i = 0; i < GLTANG_VECTORX_COUNT(block->statements); ++i) {
    GLTANG_Ast_Node * statement = (GLTANG_Ast_Node *)GLTANG_TYPEX_P(block->statements->data[i]);
    gltang_ast_node_walk(statement, callback, data, return_value);
  }
}

