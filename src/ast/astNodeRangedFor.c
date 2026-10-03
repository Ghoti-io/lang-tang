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
#include <ghoti.io/cutil/string.h>
#include <ghoti.io/cutil/hash.h>
#include <ghoti.io/lang-tang/macros.h>
#include <ghoti.io/lang-tang/ast/astNodeAssign.h>
#include <ghoti.io/lang-tang/ast/astNodeBinary.h>
#include <ghoti.io/lang-tang/ast/astNodeIdentifier.h>
#include <ghoti.io/lang-tang/ast/astNodeRangedFor.h>

GLTANG_Ast_Node_VTable gltang_ast_node_ranged_for_vtable = {
  .name = "RangedFor",
  .destroy = gltang_ast_node_ranged_for_destroy,
  .print = gltang_ast_node_ranged_for_print,
  .walk = gltang_ast_node_ranged_for_walk,
};


GLTANG_Ast_Node_Ranged_For * gltang_ast_node_ranged_for_create(const char * identifier, GLTANG_Ast_Node * expression, GLTANG_Ast_Node * block, GLTANG_PARSER_LTYPE location) {
  assert(identifier);
  assert(expression);
  assert(block);

  // Perform all of the necessary allocations or fail.
  // Allocate space for the ranged-for node.
  GLTANG_Ast_Node_Ranged_For * self = gcu_malloc(sizeof(GLTANG_Ast_Node_Ranged_For));
  if (!self) {
    goto SELF_CREATE_FAILED;
  }
  // Create an identifier node for the ranged-for loop variable.
  GLTANG_Ast_Node * identifier_node = (GLTANG_Ast_Node *)gltang_ast_node_identifier_create(identifier, location);
  if (!identifier_node) {
    goto IDENTIFIER_CREATE_FAILED;
  }
  // Create a unique name for the iterator variable.
  char * iterator_name = gcu_malloc(32);
  if (!iterator_name) {
    goto ITERATOR_NAME_CREATE_FAILED;
  }
  snprintf(iterator_name, 31, "iterator::%p", (void *)self);
  // Create an identifier node for the iterator variable.
  GLTANG_Ast_Node * iterator_node = (GLTANG_Ast_Node *)gltang_ast_node_identifier_create(iterator_name, location);
  if (!iterator_node) {
    goto ITERATOR_IDENTIFIER_CREATE_FAILED;
  }

  // All allocations are successful, so initialize the ranged-for node.
  *self = (GLTANG_Ast_Node_Ranged_For) {
    .base = {
      .vtable = &gltang_ast_node_ranged_for_vtable,
      .location = location,
      .is_singleton = false,
    },
    .identifier = identifier_node,
    .expression = expression,
    .iterator = iterator_node,
    .block = block,
  };
  return self;

  // Failure cleanup.
ITERATOR_IDENTIFIER_CREATE_FAILED:
  gcu_free(iterator_name);
ITERATOR_NAME_CREATE_FAILED:
  // A refused creation leaves every argument with the caller, and the caller
  // (the parser) frees `identifier` itself. gltang_ast_node_destroy() would
  // free the string the node adopted as well - a double free, which ctang had
  // on this path - so only the node's own storage is released here.
  gcu_free(identifier_node);
IDENTIFIER_CREATE_FAILED:
  gcu_free(self);
SELF_CREATE_FAILED:
  return 0;
}


void gltang_ast_node_ranged_for_destroy(GLTANG_Ast_Node * self) {
  assert(self);
  assert(GLTANG_AST_IS_RANGED_FOR(self));
  GLTANG_Ast_Node_Ranged_For * ranged_for = (GLTANG_Ast_Node_Ranged_For *) self;

  gltang_ast_node_destroy(ranged_for->identifier);
  gltang_ast_node_destroy(ranged_for->expression);
  gltang_ast_node_destroy(ranged_for->iterator);
  gltang_ast_node_destroy(ranged_for->block);
  gcu_free(self);
}


void gltang_ast_node_ranged_for_print(GLTANG_Ast_Node * self, const char * indent) {
  assert(self);
  assert(GLTANG_AST_IS_RANGED_FOR(self));
  GLTANG_Ast_Node_Ranged_For * ranged_for = (GLTANG_Ast_Node_Ranged_For *) self;

  assert(indent);
  size_t indent_len = strlen(indent);
  char * new_indent = gcu_malloc(indent_len + 5);
  if (!new_indent) {
    return;
  }
  memcpy(new_indent, indent, indent_len + 1);
  memcpy(new_indent + indent_len, "    ", 5);

  gltang_ast_node_print(ranged_for->identifier, indent);

  printf("%s  Expression:\n", indent);
  gltang_ast_node_print(ranged_for->expression, new_indent);

  printf("%s  Block:\n", indent);
  gltang_ast_node_print(ranged_for->block, new_indent);
  gcu_free(new_indent);
}


void gltang_ast_node_ranged_for_walk(GLTANG_Ast_Node * self, GLTANG_Ast_Node_Walk_Callback callback, void * data, void * return_value) {
  assert(self);
  assert(GLTANG_AST_IS_RANGED_FOR(self));
  GLTANG_Ast_Node_Ranged_For * ranged_for = (GLTANG_Ast_Node_Ranged_For *) self;

  callback(self, data, return_value);

  gltang_ast_node_walk(ranged_for->identifier, callback, data, return_value);
  gltang_ast_node_walk(ranged_for->expression, callback, data, return_value);
  gltang_ast_node_walk(ranged_for->iterator, callback, data, return_value);
  gltang_ast_node_walk(ranged_for->block, callback, data, return_value);
}

