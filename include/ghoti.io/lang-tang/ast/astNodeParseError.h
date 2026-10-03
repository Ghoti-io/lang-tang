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
 *
 * This file contains the definition of the class astNodeParseError.
 *
 * This class is only used to represent a parse error when trying to parse
 * code into an AST.
 */

#ifndef GHOTI_IO_GLTANG_AST_ASTNODEPARSEERROR_H
#define GHOTI_IO_GLTANG_AST_ASTNODEPARSEERROR_H

#include <ghoti.io/lang-tang/macros.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <ghoti.io/lang-tang/ast/astNode.h>

/**
 * The vtable for the GLTANG_Ast_Node_Parse_Error class.
 */
GLTANG_API_DATA extern GLTANG_Ast_Node_VTable gltang_ast_node_parse_error_vtable;


/**
 * An error representing the fact that an out of memory error occurred when
 * attempting to create a parse error.
 */
GLTANG_API_DATA extern GLTANG_Ast_Node * gltang_ast_node_parse_error_out_of_memory;


/**
 * The GLTANG_Ast_Node_Parse_Error class.
 */
struct GLTANG_Ast_Node_Parse_Error {
  /**
   * The base class.
   */
  GLTANG_Ast_Node base;
  /**
   * The message of the parse error.
   */
  char * message;
};


/**
 * Creates a new GLTANG_Ast_Node_Parse_Error object.
 *
 * @param message The message of the parse error.
 * @param location The location of the parse error in the source code.
 * @return The new GLTANG_Ast_Node_Parse_Error object or NULL on failure.
 */
GLTANG_API GLTANG_NO_DISCARD GLTANG_Ast_Node_Parse_Error * gltang_ast_node_parse_error_create(const char * message, GLTANG_PARSER_LTYPE location);


/**
 * The message of a parse-error node.
 *
 * @param node A node that GLTANG_AST_IS_PARSE_ERROR() accepts.
 * @return The message, owned by the node; never NULL.
 */
GLTANG_API const char * gltang_ast_node_parse_error_message(const GLTANG_Ast_Node * node);


/**
 * Destroys a GLTANG_Ast_Node_Parse_Error object.
 *
 * This function should not be called directly. Use gltang_ast_node_destroy()
 * instead.
 *
 * @param parse_error The GLTANG_Ast_Node_Parse_Error object to destroy.
 */
GLTANG_API void gltang_ast_node_parse_error_destroy(GLTANG_Ast_Node * parse_error);


/**
 * Prints a GLTANG_Ast_Node_Parse_Error object to stdout.
 *
 * This function should not be called directly. Use gltang_ast_node_print()
 * instead.
 *
 * @param parse_error The GLTANG_Ast_Node_Parse_Error object to print.
 * @param indent The number of spaces to indent the output.
 */
GLTANG_API void gltang_ast_node_parse_error_print(GLTANG_Ast_Node * parse_error, const char * indent);



/**
 * Walks a GLTANG_Ast_Node_Parse_Error object.
 *
 * @param parse_error The GLTANG_Ast_Node_Parse_Error object to walk.
 * @param callback The callback to call for each node in the parse error.
 * @param data The data to pass to the callback.
 * @param return_value The value to return from the walk function.
 */
GLTANG_API void gltang_ast_node_parse_error_walk(GLTANG_Ast_Node * parse_error, GLTANG_Ast_Node_Walk_Callback callback, void * data, void * return_value);


#ifdef __cplusplus
}
#endif // __cplusplus

#endif // GHOTI_IO_GLTANG_AST_ASTNODEPARSEERROR_H
