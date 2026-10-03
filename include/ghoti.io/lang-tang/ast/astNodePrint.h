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

#ifndef GHOTI_IO_GLTANG_AST_ASTNODEPRINT_H
#define GHOTI_IO_GLTANG_AST_ASTNODEPRINT_H

#include <ghoti.io/lang-tang/macros.h>

#ifdef __cplusplus
extern "C" {
#endif //__cplusplus

#include <ghoti.io/lang-tang/ast/astNode.h>

/**
 * The vtable for the GLTANG_Ast_Node_Print class.
 */
GLTANG_API_DATA extern GLTANG_Ast_Node_VTable gltang_ast_node_print_vtable;

/**
 * The GLTANG_Ast_Node_Print class.
 */
enum GLTANG_Print_Type {
  GLTANG_PRINT_TYPE_DEFAULT, ///> No type specified.
  GLTANG_PRINT_TYPE_PERCENT, ///> Use percent encoding.
  GLTANG_PRINT_TYPE_HTML,    ///> Use HTML encoding.
  GLTANG_PRINT_TYPE_JSON,    ///> Use JSON encoding.
};

/**
 * The GLTANG_Ast_Node_Print class.
 */
struct GLTANG_Ast_Node_Print {
  /**
   * The base class.
   */
  GLTANG_Ast_Node base;
  /**
   * The type of print.
   */
  GLTANG_Ast_Node * expression;
};

/**
 * Creates a new GLTANG_Ast_Node_Print object.
 *
 * @param expression The expression to print.
 * @param location The location of the print in the source code.
 * @return The new GLTANG_Ast_Node_Print object or NULL on failure.
 */
GLTANG_API GLTANG_NO_DISCARD GLTANG_Ast_Node_Print * gltang_ast_node_print_create(GLTANG_Ast_Node * expression, GLTANG_PARSER_LTYPE location);

/**
 * Destroys a GLTANG_Ast_Node_Print object.
 *
 * This function should not be called directly. Use gltang_ast_node_destroy()
 * instead.
 *
 * @param self The GLTANG_Ast_Node_Print object to destroy.
 */
GLTANG_API void gltang_ast_node_print_destroy(GLTANG_Ast_Node * self);

/**
 * Prints a GLTANG_Ast_Node_Print object to stdout.
 *
 * This function should not be called directly. Use gltang_ast_node_print()
 * instead.
 *
 * @param self The GLTANG_Ast_Node_Print object to print.
 * @param indent The string to print before each line of output.
 */
GLTANG_API void gltang_ast_node_print_print(GLTANG_Ast_Node * self, const char * indent);

/**
 * Walks a GLTANG_Ast_Node_Print object.
 *
 * This function should not be called directly. Use gltang_ast_node_walk()
 * instead.
 *
 * @param self The GLTANG_Ast_Node_Print object to walk.
 * @param callback The callback to call for each node.
 * @param data The user-defined data to pass to the callback.
 * @param return_value The return value of the walk, populated by the callback.
 */
GLTANG_API void gltang_ast_node_print_walk(GLTANG_Ast_Node * self, GLTANG_Ast_Node_Walk_Callback callback, void * data, void * return_value);

#ifdef __cplusplus
}
#endif //__cplusplus

#endif //GHOTI_IO_GLTANG_AST_ASTNODEPRINT_H
