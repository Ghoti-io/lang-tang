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
#include <ghoti.io/lang-tang/macros.h>
#include <ghoti.io/lang-tang/parse.h>
#include "ast_internal.h"
#include <ghoti.io/lang-tang/ast/astNodeAssign.h>
#include <ghoti.io/lang-tang/ast/astNodeBinary.h>
#include <ghoti.io/lang-tang/ast/astNodeFor.h>
#include <ghoti.io/lang-tang/ast/astNodeIdentifier.h>


// Helper macro to determine if a location indicates that the node was
// actually present in the code (as opposed to being added by the parser as a
// default value).
#define LOCATION_IS_PRESENT(location) ((location).first_line || (location).first_column || (location).last_line || (location).last_column)


GLTANG_Ast_Node_VTable gltang_ast_node_for_vtable = {
  .name = "For",
  .destroy = gltang_ast_node_for_destroy,
  .print = gltang_ast_node_for_print,
  .walk = gltang_ast_node_for_walk,
};


GLTANG_Ast_Node_For * gltang_ast_node_for_create(GLTANG_Ast_Node * init, GLTANG_Ast_Node * condition, GLTANG_Ast_Node * update, GLTANG_Ast_Node * block, GLTANG_PARSER_LTYPE location) {
  assert(init);
  assert(condition);
  assert(update);
  assert(block);

  uint32_t depth = 1 + gltang_ast_height_max(gltang_ast_height_max(gltang_ast_height_max(gltang_ast_height(init), gltang_ast_height(condition)), gltang_ast_height(update)), gltang_ast_height(block));

  if (!gltang_ast_depth_ok(depth)) {

    return 0;

  }


  GLTANG_Ast_Node_For * self = gcu_malloc(sizeof(GLTANG_Ast_Node_For));
  if (!self) {
    return 0;
  }

  *self = (GLTANG_Ast_Node_For) {
    .base = {
      .vtable = &gltang_ast_node_for_vtable,
      .location = location,
      .is_singleton = false,
      .depth = depth,
    },
    .init = init,
    .condition = condition,
    .update = update,
    .block = block,
  };
  return self;
}


void gltang_ast_node_for_destroy(GLTANG_Ast_Node * self) {
  assert(self);
  assert(GLTANG_AST_IS_FOR(self));
  GLTANG_Ast_Node_For * for_node = (GLTANG_Ast_Node_For *) self;

  gltang_ast_node_destroy(for_node->init);
  gltang_ast_node_destroy(for_node->condition);
  gltang_ast_node_destroy(for_node->update);
  gltang_ast_node_destroy(for_node->block);
  gcu_free(self);
}


void gltang_ast_node_for_print(GLTANG_Ast_Node * self, const char * indent) {
  assert(self);
  assert(GLTANG_AST_IS_FOR(self));
  GLTANG_Ast_Node_For * for_node = (GLTANG_Ast_Node_For *) self;

  assert(indent);
  char * new_indent = gltang_ast_indent_extend(indent, 4);
  if (!new_indent) {
    return;
  }

  assert(self->vtable);
  assert(self->vtable->name);
  printf("%s%s\n", indent, self->vtable->name);

  printf("%s  Init:\n", indent);
  gltang_ast_node_print(for_node->init, new_indent);

  printf("%s  Condition:\n", indent);
  gltang_ast_node_print(for_node->condition, new_indent);

  printf("%s  Update:\n", indent);
  gltang_ast_node_print(for_node->update, new_indent);

  printf("%s  Block:\n", indent);
  gltang_ast_node_print(for_node->block, new_indent);
  gltang_ast_indent_release(new_indent, indent);
}


void gltang_ast_node_for_walk(GLTANG_Ast_Node * self, GLTANG_Ast_Node_Walk_Callback callback, void * data, void * return_value) {
  assert(self);
  assert(GLTANG_AST_IS_FOR(self));
  GLTANG_Ast_Node_For * for_node = (GLTANG_Ast_Node_For *) self;

  callback(self, data, return_value);

  gltang_ast_node_walk(for_node->init, callback, data, return_value);
  gltang_ast_node_walk(for_node->condition, callback, data, return_value);
  gltang_ast_node_walk(for_node->update, callback, data, return_value);
  gltang_ast_node_walk(for_node->block, callback, data, return_value);
}

