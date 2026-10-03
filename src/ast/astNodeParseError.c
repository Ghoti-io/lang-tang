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
#include <ghoti.io/lang-tang/ast/astNodeParseError.h>

GLTANG_Ast_Node_VTable gltang_ast_node_parse_error_vtable = {
  .name = "Parse Error",
  .destroy = gltang_ast_node_parse_error_destroy,
  .print = gltang_ast_node_parse_error_print,
  .walk = gltang_ast_node_parse_error_walk,
};


static GLTANG_Ast_Node_Parse_Error gltang_ast_node_parse_error_out_of_memory_singleton = {
  .base = {
    .vtable = &gltang_ast_node_parse_error_vtable,
    .location = {0, 0, 0, 0},
    .is_singleton = true,
  },
  .message = "An out of memory error ocurred when attempting to create a parse error.",
};
GLTANG_Ast_Node * gltang_ast_node_parse_error_out_of_memory = (GLTANG_Ast_Node *) &gltang_ast_node_parse_error_out_of_memory_singleton;


GLTANG_Ast_Node_Parse_Error * gltang_ast_node_parse_error_create(const char * message, GLTANG_PARSER_LTYPE location) {
  assert(message);

  GLTANG_Ast_Node_Parse_Error * self = gcu_malloc(sizeof(GLTANG_Ast_Node_Parse_Error));
  if (!self) {
    return 0;
  }

  *self = (GLTANG_Ast_Node_Parse_Error) {
    .base = {
      .vtable = &gltang_ast_node_parse_error_vtable,
      .location = location,
      .is_singleton = false,
    },
    .message = 0,
  };

  size_t messageLength = strlen(message);
  self->message = gcu_malloc(messageLength + 1);
  if (!self->message) {
    gcu_free(self);
    return 0;
  }
  strcpy(self->message, message);

  return self;
}


const char * gltang_ast_node_parse_error_message(const GLTANG_Ast_Node * node) {
  assert(node);
  assert(GLTANG_AST_IS_PARSE_ERROR(node));
  const GLTANG_Ast_Node_Parse_Error * error = (const GLTANG_Ast_Node_Parse_Error *) node;
  return error->message ? error->message : "";
}


void gltang_ast_node_parse_error_destroy(GLTANG_Ast_Node * self) {
  assert(self);
  assert(GLTANG_AST_IS_PARSE_ERROR(self));
  GLTANG_Ast_Node_Parse_Error * parseError = (GLTANG_Ast_Node_Parse_Error *) self;

  gcu_free(parseError->message);
  gcu_free(parseError);
}


void gltang_ast_node_parse_error_print(GLTANG_Ast_Node * self, const char * indent) {
  assert(self);
  assert(GLTANG_AST_IS_PARSE_ERROR(self));
  GLTANG_Ast_Node_Parse_Error * parseError = (GLTANG_Ast_Node_Parse_Error *) self;

  assert(indent);
  assert(self->vtable);
  assert(self->vtable->name);
  printf("%sParse Error: %s\n", indent, parseError->message);
}


void gltang_ast_node_parse_error_walk(GLTANG_Ast_Node * self, GLTANG_Ast_Node_Walk_Callback callback, void * data, void * return_value) {
  assert(self);
  callback(self, data, return_value);
}
