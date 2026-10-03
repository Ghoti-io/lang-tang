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
#include <ghoti.io/lang-tang/parse.h>
#include "ast_internal.h"
#include <ghoti.io/lang-tang/ast/astNodeMap.h>
#include <ghoti.io/lang-tang/ast/astNodeString.h>
#include <ghoti.io/lang-tang/unicodeString.h>

GLTANG_Ast_Node_VTable gltang_ast_node_map_vtable = {
  .name = "Map",
  .destroy = gltang_ast_node_map_destroy,
  .print = gltang_ast_node_map_print,
  .walk = gltang_ast_node_map_walk,
};


GLTANG_Ast_Node_Map * gltang_ast_node_map_create(GLTANG_VectorX * pairs, GLTANG_PARSER_LTYPE location) {
  assert(pairs);

  uint32_t depth = 1 + gltang_ast_height_of_pairs(pairs);

  if (!gltang_ast_depth_ok(depth)) {

    return 0;

  }


  GLTANG_Ast_Node_Map * self = gcu_malloc(sizeof(GLTANG_Ast_Node_Map));
  if (!self) {
    return 0;
  }

  *self = (GLTANG_Ast_Node_Map) {
    .base = {
      .vtable = &gltang_ast_node_map_vtable,
      .location = location,
      .is_singleton = false,
      .depth = depth,
    },
    .pairs = pairs,
  };
  return self;
}


void gltang_ast_node_map_destroy(GLTANG_Ast_Node * self) {
  assert(self);
  assert(GLTANG_AST_IS_MAP(self));
  GLTANG_Ast_Node_Map * map = (GLTANG_Ast_Node_Map *)self;

  GLTANG_VECTORX_DESTROY(map->pairs);
  gcu_free(self);
}


void gltang_ast_node_map_print(GLTANG_Ast_Node * self, const char * indent) {
  assert(self);
  assert(GLTANG_AST_IS_MAP(self));
  GLTANG_Ast_Node_Map * map = (GLTANG_Ast_Node_Map *)self;

  assert(indent);
  char * new_indent = gltang_ast_indent_extend(indent, 4);
  if (!new_indent) {
    return;
  }

  assert(self->vtable);
  assert(self->vtable->name);
  printf("%s%s\n", indent, self->vtable->name);

  assert(map->pairs);
  assert(map->pairs->count ? (bool)map->pairs->data : true);
  for (size_t i = 0; i < GLTANG_VECTORX_COUNT(map->pairs); ++i) {
    GLTANG_Ast_Node_Map_Pair * pair = (GLTANG_Ast_Node_Map_Pair *)GLTANG_TYPEX_P(map->pairs->data[i]);
    printf("%s  Key:\n", indent);
    gltang_ast_node_print(pair->key, new_indent);
    printf("%s  Value:\n", indent);
    gltang_ast_node_print(pair->value, new_indent);
  }
  gltang_ast_indent_release(new_indent, indent);
}


void gltang_ast_node_map_walk(GLTANG_Ast_Node * self, GLTANG_Ast_Node_Walk_Callback callback, void * data, void * return_value) {
  assert(self);
  assert(GLTANG_AST_IS_MAP(self));
  GLTANG_Ast_Node_Map * map = (GLTANG_Ast_Node_Map *)self;

  callback(self, data, return_value);

  assert(map->pairs);
  assert(map->pairs->count ? (bool)map->pairs->data : true);
  for (size_t i = 0; i < GLTANG_VECTORX_COUNT(map->pairs); ++i) {
    GLTANG_Ast_Node_Map_Pair * pair = (GLTANG_Ast_Node_Map_Pair *)GLTANG_TYPEX_P(map->pairs->data[i]);
    callback(pair->key, data, return_value);
    callback(pair->value, data, return_value);
  }
}

