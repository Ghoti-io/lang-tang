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
#include <ghoti.io/lang-tang/ast/astNode.h>
#include <ghoti.io/lang-tang/ast/astNodeMap.h>
#include <ghoti.io/lang-tang/parse.h>
#include "ast_internal.h"

GLTANG_Ast_Node_VTable gltang_ast_node_null_vtable = {
  .name = "Null",
  .destroy = gltang_ast_node_null_destroy,
  .print = gltang_ast_node_null_print,
  .walk = gltang_ast_node_null_walk,
};


GLTANG_Ast_Node * gltang_ast_node_create(GLTANG_PARSER_LTYPE location) {
  GLTANG_Ast_Node * self = gcu_malloc(sizeof(GLTANG_Ast_Node));
  if (!self) {
    return 0;
  }
  *self = (GLTANG_Ast_Node) {
    .vtable = &gltang_ast_node_null_vtable,
    .location = location,
    .is_singleton = false,
    .depth = 1,
  };
  return self;
}


void gltang_ast_node_destroy(GLTANG_Ast_Node * self) {
  assert(self);
  assert(self->vtable);

  // A singleton has static storage, so there is nothing to give back.  The
  // is_singleton test used to be folded into the condition below, which sent
  // singletons to the fallback - and the fallback is a free(), not a no-op.
  // So the check that exists to protect a singleton was what freed it, and
  // every error reported by returning a parse-error singleton aborted the
  // process with "free(): invalid pointer" instead of failing compilation.
  if (self->is_singleton) {
    return;
  }

  self->vtable->destroy
    ? self->vtable->destroy(self)
    : gltang_ast_node_null_destroy(self);
}


void gltang_ast_node_print(GLTANG_Ast_Node * self, const char * indent) {
  assert(self);
  assert(self->vtable);
  self->vtable->print
    ? self->vtable->print(self, indent)
    : gltang_ast_node_null_print(self, indent);
}


void gltang_ast_node_walk(GLTANG_Ast_Node * self, GLTANG_Ast_Node_Walk_Callback callback, void * data, void * return_value) {
  assert(self);
  assert(self->vtable);
  self->vtable->walk
    ? self->vtable->walk(self, callback, data, return_value)
    : gltang_ast_node_null_walk(self, callback, data, return_value);
}


void gltang_ast_node_null_destroy(GLTANG_Ast_Node * self) {
  gcu_free(self);
}


void gltang_ast_node_null_print(GLTANG_Ast_Node * self, const char * indent) {
  assert(self);
  assert(indent);
  assert(self->vtable);
  printf("%s%s\n", indent, self->vtable->name);
}


void gltang_ast_node_null_walk(GLTANG_Ast_Node * self, GLTANG_Ast_Node_Walk_Callback callback, void * data, void * return_value) {
  callback(self, data, return_value);
}


// The refusal is thread-local because a parse runs on one thread and the
// constructors it calls have no parse context to report to.
static _Thread_local bool depth_refused = false;

uint32_t gltang_ast_height_of_nodes(const GLTANG_VectorX * nodes) {
  uint32_t height = 0;
  if (!nodes) {
    return 0;
  }
  size_t count = GLTANG_VECTORX_COUNT((GLTANG_VectorX *)nodes);
  for (size_t i = 0; i < count; ++i) {
    height = gltang_ast_height_max(height, gltang_ast_height((const GLTANG_Ast_Node *)GLTANG_TYPEX_P(nodes->data[i])));
  }
  return height;
}

uint32_t gltang_ast_height_of_pairs(const GLTANG_VectorX * pairs) {
  uint32_t height = 0;
  if (!pairs) {
    return 0;
  }
  size_t count = GLTANG_VECTORX_COUNT((GLTANG_VectorX *)pairs);
  for (size_t i = 0; i < count; ++i) {
    const GLTANG_Ast_Node_Map_Pair * pair = GLTANG_TYPEX_P(pairs->data[i]);
    if (pair) {
      height = gltang_ast_height_max(height, gltang_ast_height_max(gltang_ast_height(pair->key), gltang_ast_height(pair->value)));
    }
  }
  return height;
}

bool gltang_ast_depth_ok(uint32_t height) {
  if (height > GLTANG_MAX_TREE_DEPTH) {
    depth_refused = true;
    return false;
  }
  return true;
}

void gltang_ast_depth_reset(void) {
  depth_refused = false;
}

bool gltang_ast_depth_refused(void) {
  return depth_refused;
}


char * gltang_ast_indent_extend(const char * indent, size_t extra) {
  size_t length = strlen(indent);
  if (length >= GLTANG_PRINT_INDENT_MAX) {
    return (char *)indent;
  }
  char * extended = gcu_malloc(length + extra + 1);
  if (!extended) {
    return 0;
  }
  memcpy(extended, indent, length);
  memset(extended + length, ' ', extra);
  extended[length + extra] = '\0';
  return extended;
}

void gltang_ast_indent_release(char * extended, const char * indent) {
  if (extended && extended != indent) {
    gcu_free(extended);
  }
}
