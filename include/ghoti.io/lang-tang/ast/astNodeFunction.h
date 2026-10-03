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

#ifndef GHOTI_IO_GLTANG_AST_ASTNODEFUNCTION_H
#define GHOTI_IO_GLTANG_AST_ASTNODEFUNCTION_H

#include <ghoti.io/lang-tang/macros.h>

#ifdef __cplusplus
extern "C" {
#endif //__cplusplus

#include <ghoti.io/lang-tang/ast/astNode.h>

/**
 * The vtable for the GLTANG_Ast_Node_Function class.
 */
GLTANG_API_DATA extern GLTANG_Ast_Node_VTable gltang_ast_node_function_vtable;

/**
 * The GLTANG_Ast_Node_Function class.
 *
 * Functions may be declared in any scope, but they are only callable from the
 * scope in which they are declared, or from a child scope of that scope.
 *
 * A function declaration will cause all identifiers in that scope or child
 * scopes to be resolved to the function declaration.
 *
 * Function identifiers of the same name, but in different scopes are treated
 * separately and will resolve to the correct function declaration according to
 * the scope resolution rules.
 */
struct GLTANG_Ast_Node_Function {
  /**
   * The base class.
   */
  GLTANG_Ast_Node base;
  /**
   * The identifier of the function.
   */
  const char * identifier;
  /**
   * A hash of the identifier.
   */
  GLTANG_UInteger hash;
  /**
   * The parameters of the function.
   *
   * This is a vector of AST Node Identifiers.
   */
  GLTANG_VectorX * parameters;
  /**
   * The block of the function.
   */
  GLTANG_Ast_Node * block;
};

/**
 * Creates a new GLTANG_Ast_Node_Function object.
 *
 * @param identifier The identifier of the function.
 * @param parameters The parameters of the function.
 * @param block The block of the function.
 * @param location The location of the function in the source code.
 * @return The new GLTANG_Ast_Node_Function object or NULL on failure.
 */
GLTANG_API GLTANG_NO_DISCARD GLTANG_Ast_Node_Function * gltang_ast_node_function_create(const char * identifier, GLTANG_VectorX * parameters, GLTANG_Ast_Node * block, GLTANG_PARSER_LTYPE location);

/**
 * Destroys a GLTANG_Ast_Node_Function object.
 *
 * This function should not be called directly. Use gltang_ast_node_destroy()
 * instead.
 *
 * @param self The GLTANG_Ast_Node_Function object to destroy.
 */
GLTANG_API void gltang_ast_node_function_destroy(GLTANG_Ast_Node * self);

/**
 * Prints a GLTANG_Ast_Node_Function object to stdout.
 *
 * This function should not be called directly. Use gltang_ast_node_print()
 * instead.
 *
 * @param self The GLTANG_Ast_Node_Function object to print.
 * @param indent The string to print before each line of output.
 */
GLTANG_API void gltang_ast_node_function_print(GLTANG_Ast_Node * self, const char * indent);

/**
 * Walks a GLTANG_Ast_Node_Function object.
 *
 * This function should not be called directly. Use gltang_ast_node_walk()
 * instead.
 *
 * @param self The GLTANG_Ast_Node_Function object to walk.
 * @param callback The callback function to call for each node.
 * @param data The user-defined data to pass to the callback function.
 * @param return_value The return value of the walk, populated by the callback.
 */
GLTANG_API void gltang_ast_node_function_walk(GLTANG_Ast_Node * self, GLTANG_Ast_Node_Walk_Callback callback, void * data, void * return_value);

#ifdef __cplusplus
}
#endif //__cplusplus

#endif //GHOTI_IO_GLTANG_AST_ASTNODEFUNCTION_H
