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

#ifndef GHOTI_IO_GLTANG_AST_ASTNODEIFELSE_H
#define GHOTI_IO_GLTANG_AST_ASTNODEIFELSE_H

#include <ghoti.io/lang-tang/macros.h>

#ifdef __cplusplus
extern "C" {
#endif //__cplusplus

#include <ghoti.io/lang-tang/ast/astNode.h>

/**
 * The vtable for the GLTANG_Ast_Node_If_Else class.
 */
GLTANG_API_DATA extern GLTANG_Ast_Node_VTable gltang_ast_node_if_else_vtable;

/**
 * The GLTANG_Ast_Node_If_Else class.
 */
struct GLTANG_Ast_Node_If_Else {
  /**
   * The base class.
   */
  GLTANG_Ast_Node base;
  /**
   * The condition of the if-else statement.
   */
  GLTANG_Ast_Node * condition;
  /**
   * The block of the if-else statement.
   */
  GLTANG_Ast_Node * ifBlock;
  /**
   * The else block of the if-else statement.
   */
  GLTANG_Ast_Node * elseBlock;
};

/**
 * Creates a new GLTANG_Ast_Node_If_Else object.
 *
 * @param condition The condition of the if-else statement.
 * @param ifBlock The block of the if-else statement.
 * @param elseBlock The else block of the if-else statement.
 * @param location The location of the if-else statement in the source code.
 * @return The new GLTANG_Ast_Node_If_Else object or NULL on failure.
 */
GLTANG_API GLTANG_NO_DISCARD GLTANG_Ast_Node_If_Else * gltang_ast_node_if_else_create(GLTANG_Ast_Node * condition, GLTANG_Ast_Node * ifBlock, GLTANG_Ast_Node * elseBlock, GLTANG_PARSER_LTYPE location);

/**
 * Destroys a GLTANG_Ast_Node_If_Else object.
 *
 * This function should not be called directly. Use gltang_ast_node_destroy()
 * instead.
 *
 * @param self The GLTANG_Ast_Node_If_Else object to destroy.
 */
GLTANG_API void gltang_ast_node_if_else_destroy(GLTANG_Ast_Node * self);

/**
 * Prints a GLTANG_Ast_Node_If_Else object to stdout.
 *
 * This function should not be called directly. Use gltang_ast_node_print()
 * instead.
 *
 * @param self The GLTANG_Ast_Node_If_Else object to print.
 * @param indent The string to print before each line of output.
 */
GLTANG_API void gltang_ast_node_if_else_print(GLTANG_Ast_Node * self, const char * indent);

/**
 * Walks a GLTANG_Ast_Node_If_Else object.
 *
 * This function should not be called directly. Use gltang_ast_node_walk()
 * instead.
 *
 * @param self The GLTANG_Ast_Node_If_Else object to walk.
 * @param callback The callback to call for each node in the tree.
 * @param data The user-defined data to pass to the callback.
 * @param return_value The return value of the walk, populated by the callback.
 */
GLTANG_API void gltang_ast_node_if_else_walk(GLTANG_Ast_Node * self, GLTANG_Ast_Node_Walk_Callback callback, void * data, void * return_value);

#ifdef __cplusplus
}
#endif //__cplusplus

#endif //GHOTI_IO_GLTANG_AST_ASTNODEIFELSE_H
