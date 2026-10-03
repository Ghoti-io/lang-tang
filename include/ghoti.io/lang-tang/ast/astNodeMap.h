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

#ifndef GHOTI_IO_GLTANG_AST_ASTNODEMAP_H
#define GHOTI_IO_GLTANG_AST_ASTNODEMAP_H

#include <ghoti.io/lang-tang/macros.h>

#ifdef __cplusplus
extern "C" {
#endif //__cplusplus

#include <ghoti.io/lang-tang/ast/astNode.h>

/**
 * The vtable for the GLTANG_Ast_Node_Map class.
 */
GLTANG_API_DATA extern GLTANG_Ast_Node_VTable gltang_ast_node_map_vtable;

/**
 * For "map" expressions, we need to store the key-value pairs in a vector.
 */
struct GLTANG_Ast_Node_Map_Pair {
  GLTANG_Ast_Node * key;    ///> The key of the pair.
  GLTANG_Ast_Node * value;  ///> The value of the pair.
};

/**
 * The GLTANG_Ast_Node_Map class.
 */
struct GLTANG_Ast_Node_Map {
  /**
   * The base class.
   */
  GLTANG_Ast_Node base;
  /**
   * The key-value pairs of the map.
   *
   * This is a vector of GLTANG_Ast_Node_Map_Pair objects.
   */
  GLTANG_VectorX * pairs;
};

/**
 * Creates a new GLTANG_Ast_Node_Map object.
 *
 * @param pairs The key-value pairs of the map.
 * @param location The location of the map in the source code.
 * @return The new GLTANG_Ast_Node_Map object or NULL on failure.
 */
GLTANG_API GLTANG_NO_DISCARD GLTANG_Ast_Node_Map * gltang_ast_node_map_create(GLTANG_VectorX * pairs, GLTANG_PARSER_LTYPE location);

/**
 * Destroys a GLTANG_Ast_Node_Map object.
 *
 * This function should not be called directly. Use gltang_ast_node_destroy()
 * instead.
 *
 * @param self The GLTANG_Ast_Node_Map object to destroy.
 */
GLTANG_API void gltang_ast_node_map_destroy(GLTANG_Ast_Node * self);

/**
 * Prints a GLTANG_Ast_Node_Map object to stdout.
 *
 * This function should not be called directly. Use gltang_ast_node_print()
 * instead.
 *
 * @param self The GLTANG_Ast_Node_Map object to print.
 * @param indent The string to print before each line of output.
*/
GLTANG_API void gltang_ast_node_map_print(GLTANG_Ast_Node * self, const char * indent);

/**
 * Walks a GLTANG_Ast_Node_Map object.
 *
 * This function should not be called directly. Use gltang_ast_node_walk()
 * instead.
 *
 * @param self The GLTANG_Ast_Node_Map object to walk.
 * @param callback The callback function to call for each node in the map.
 * @param data The data to pass to the callback function.
 * @param return_value The return value of the walk, populated by the callback.
 */
GLTANG_API void gltang_ast_node_map_walk(GLTANG_Ast_Node * self, GLTANG_Ast_Node_Walk_Callback callback, void * data, void * return_value);

#ifdef __cplusplus
}
#endif //__cplusplus

#endif //GHOTI_IO_GLTANG_AST_ASTNODEMAP_H
