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

/**
 * @file
 * @stability free
 */

#ifndef GHOTI_IO_GLTANG_AST_ASTNODEUSE_H
#define GHOTI_IO_GLTANG_AST_ASTNODEUSE_H

#include <ghoti.io/lang-tang/macros.h>

#ifdef __cplusplus
extern "C" {
#endif //__cplusplus

#include <ghoti.io/lang-tang/ast/astNode.h>

/**
 * The vtable for the GLTANG_Ast_Node_Use class.
 */
GLTANG_API_DATA extern GLTANG_Ast_Node_VTable gltang_ast_node_use_vtable;

/**
 * The GLTANG_Ast_Node_Use class.
 */
struct GLTANG_Ast_Node_Use {
  /**
   * The base class.
   */
  GLTANG_Ast_Node base;
  /**
   * The identifier of the use statement.
   */
  const char * identifier;
  /**
   * The hash of the identifier.
   */
  GLTANG_UInteger hash;
  /**
   * The expression of the use statement.
   */
  GLTANG_Ast_Node * expression;
};

/**
 * Creates a new GLTANG_Ast_Node_Use object.
 *
 * @param identifier The identifier of the use statement.
 * @param expression The expression of the use statement.
 * @param location The location of the use statement in the source code.
 * @return The new GLTANG_Ast_Node_Use object or NULL on failure.
 */
GLTANG_API GLTANG_NO_DISCARD GLTANG_Ast_Node_Use * gltang_ast_node_use_create(const char * identifier, GLTANG_Ast_Node * expression, GLTANG_PARSER_LTYPE location);

/**
 * Destroys a GLTANG_Ast_Node_Use object.
 *
 * This function should not be called directly. Use gltang_ast_node_destroy()
 * instead.
 *
 * @param self The GLTANG_Ast_Node_Use object to destroy.
 */
GLTANG_API void gltang_ast_node_use_destroy(GLTANG_Ast_Node * self);

/**
 * Prints a GLTANG_Ast_Node_Use object to stdout.
 *
 * This function should not be called directly. Use gltang_ast_node_print()
 * instead.
 *
 * @param self The GLTANG_Ast_Node_Use object to print.
 * @param indent The string to print before each line of output.
 */
GLTANG_API void gltang_ast_node_use_print(GLTANG_Ast_Node * self, const char * indent);

/**
 * Walks a GLTANG_Ast_Node_Use object.
 *
 * This function should not be called directly. Use gltang_ast_node_walk()
 * instead.
 *
 * @param self The GLTANG_Ast_Node_Use object to walk.
 * @param callback The callback to call for each node in the use statement.
 * @param data The user-defined data to pass to the callback.
 * @param return_value The return value of the walk, populated by the callback.
 */
GLTANG_API void gltang_ast_node_use_walk(GLTANG_Ast_Node * self, GLTANG_Ast_Node_Walk_Callback callback, void * data, void * return_value);

#ifdef __cplusplus
}
#endif //__cplusplus

#endif //GHOTI_IO_GLTANG_AST_ASTNODEUSE_H
