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

#ifndef GHOTI_IO_GLTANG_AST_ASTNODEIDENTIFIER_H
#define GHOTI_IO_GLTANG_AST_ASTNODEIDENTIFIER_H

#include <ghoti.io/lang-tang/macros.h>

#ifdef __cplusplus
extern "C" {
#endif //__cplusplus

#include <ghoti.io/lang-tang/ast/astNode.h>

/**
 * The vtable for the GLTANG_Ast_Node_Identifier class.
 */
GLTANG_API_DATA extern GLTANG_Ast_Node_VTable gltang_ast_node_identifier_vtable;

/**
 * The GLTANG_Ast_Node_Identifier class.
 *
 * There are 3 scope types that an identifier can be:
 * 1. Local variable
 * 2. Function declaration
 * 3. Global/Library variable
 *
 * Resolution rules are as follows:
 * 1. If the identifier is a function declaration, then it is resolved to the
 *   function declaration.
 * 2. If the identifier is declared Global in the current scope, then it is
 *   resolved to the global variable.  (The outermost scope is the global scope
 *   by default.)
 * 3. Otherwise, the identifier is resolved to the local variable.
 */
struct GLTANG_Ast_Node_Identifier {
  /**
   * The base class.
   */
  GLTANG_Ast_Node base;
  /**
   * The identifier.
   */
  const char * identifier;
  /**
   * A hash of the identifier.
   */
  GLTANG_UInteger hash;
};

/**
 * Creates a new GLTANG_Ast_Node_Identifier object.
 *
 * The identifier will be adopted by the new object, so the caller should not
 * free the identifier after calling this function.
 *
 * @param identifier The identifier.
 * @param location The location of the identifier in the source code.
 * @return The new GLTANG_Ast_Node_Identifier object or NULL on failure.
 */
GLTANG_API GLTANG_NO_DISCARD GLTANG_Ast_Node_Identifier * gltang_ast_node_identifier_create(const char * identifier, GLTANG_PARSER_LTYPE location);

/**
 * Destroys a GLTANG_Ast_Node_Identifier object.
 *
 * This function should not be called directly. Use gltang_ast_node_destroy()
 * instead.
 *
 * @param self The GLTANG_Ast_Node_Identifier object to destroy.
 */
GLTANG_API void gltang_ast_node_identifier_destroy(GLTANG_Ast_Node * self);

/**
 * Prints a GLTANG_Ast_Node_Identifier object to stdout.
 *
 * This function should not be called directly. Use gltang_ast_node_print()
 * instead.
 *
 * @param self The GLTANG_Ast_Node_Identifier object to print.
 * @param indent The string to print before each line of output.
 */
GLTANG_API void gltang_ast_node_identifier_print(GLTANG_Ast_Node * self, const char * indent);

/**
 * Walks a GLTANG_Ast_Node_Identifier object.
 *
 * This function should not be called directly. Use gltang_ast_node_walk()
 * instead.
 *
 * @param self The GLTANG_Ast_Node_Identifier object to walk.
 * @param callback The callback function to call for each node.
 * @param data The user-defined data to pass to the callback function.
 * @param return_value The return value of the walk, populated by the callback.
 */
GLTANG_API void gltang_ast_node_identifier_walk(GLTANG_Ast_Node * self, GLTANG_Ast_Node_Walk_Callback callback, void * data, void * return_value);

#ifdef __cplusplus
}
#endif //__cplusplus

#endif //GHOTI_IO_GLTANG_AST_ASTNODEIDENTIFIER_H
