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
#include <ghoti.io/lang-tang/parse.h>
#include "ast_internal.h"
#include <ghoti.io/lang-tang/ast/astNodeAssign.h>
#include <ghoti.io/lang-tang/ast/astNodeGlobal.h>
#include <ghoti.io/lang-tang/ast/astNodeIdentifier.h>
#include <ghoti.io/lang-tang/ast/astNodeParseError.h>

GLTANG_Ast_Node_VTable gltang_ast_node_global_vtable = {
  .name = "Global",
  .destroy = gltang_ast_node_global_destroy,
  .print = gltang_ast_node_global_print,
  .walk = gltang_ast_node_global_walk,
};


GLTANG_Ast_Node_Global * gltang_ast_node_global_create(GLTANG_Ast_Node * identifier, GLTANG_Ast_Node * assignment, GLTANG_PARSER_LTYPE location) {
  assert(identifier);
  // NOTE: assignment can be NULL.
  GLTANG_Ast_Node * full_assignment = 0;
  if (assignment) {
    full_assignment = (GLTANG_Ast_Node *)gltang_ast_node_assign_create(identifier, assignment, location);
    if (!full_assignment) {
      return 0;
    }
  }

  uint32_t depth = 1 + gltang_ast_height(full_assignment ? full_assignment : identifier);

  if (!gltang_ast_depth_ok(depth)) {

    if (full_assignment) {

      gcu_free(full_assignment);

    }

    return 0;

  }


  GLTANG_Ast_Node_Global * self = gcu_malloc(sizeof(GLTANG_Ast_Node_Global));
  if (!self) {
    // A refused creation leaves every argument with the caller, which frees
    // `identifier` and `assignment` itself. Destroying the assignment node
    // would free both a second time (ctang did, and the parser then used
    // them), so only the node's own storage is released.
    if (full_assignment) {
      gcu_free(full_assignment);
    }
    return 0;
  }

  *self = (GLTANG_Ast_Node_Global) {
    .base = {
      .vtable = &gltang_ast_node_global_vtable,
      .location = location,
      .is_singleton = false,
      .depth = depth,
    },
    .identifier = identifier,
    .assignment = full_assignment,
  };
  return self;
}


void gltang_ast_node_global_destroy(GLTANG_Ast_Node * self) {
  assert(self);
  assert(GLTANG_AST_IS_GLOBAL(self));
  GLTANG_Ast_Node_Global * global = (GLTANG_Ast_Node_Global *) self;

  if (global->assignment) {
    gltang_ast_node_destroy(global->assignment);
  }
  else {
    gltang_ast_node_destroy(global->identifier);
  }
  gcu_free(self);
}


void gltang_ast_node_global_print(GLTANG_Ast_Node * self, const char * indent) {
  assert(self);
  assert(GLTANG_AST_IS_GLOBAL(self));
  GLTANG_Ast_Node_Global * global = (GLTANG_Ast_Node_Global *) self;

  assert(indent);
  char * new_indent = gltang_ast_indent_extend(indent, 4);
  if (!new_indent) {
    return;
  }

  assert(self->vtable);
  assert(self->vtable->name);
  assert(global->identifier);
  assert(GLTANG_AST_IS_IDENTIFIER(global->identifier));
  printf("%s%s : %s\n", indent, self->vtable->name, ((GLTANG_Ast_Node_Identifier *)global->identifier)->identifier);
  if (global->assignment) {
    printf("%s  Assignment:\n", indent);
    gltang_ast_node_print(global->assignment, new_indent);
  }
  gltang_ast_indent_release(new_indent, indent);
}


void gltang_ast_node_global_walk(GLTANG_Ast_Node * self, GLTANG_Ast_Node_Walk_Callback callback, void * data, void * return_value) {
  assert(self);
  assert(GLTANG_AST_IS_GLOBAL(self));
  GLTANG_Ast_Node_Global * global = (GLTANG_Ast_Node_Global *) self;

  callback(self, data, return_value);

  gltang_ast_node_walk(global->identifier, callback, data, return_value);
  if (global->assignment) {
    gltang_ast_node_walk(global->assignment, callback, data, return_value);
  }
}

