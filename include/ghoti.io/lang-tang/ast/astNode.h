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
 * The AST node base class, its vtable, and the types the node headers share.
 *
 * Ported from ctang's astNode.h with everything that belongs to compilation
 * removed (bytecode and binary emission, simplification, analysis, possible
 * types). What remains is what parsing and any later reader of the tree needs:
 * `name`, `destroy`, `print` and `walk`. The compiler of a later story reads
 * these headers, so the shape may change; hence the `free` label.
 */

#ifndef GHOTI_IO_GLTANG_AST_ASTNODE_H
#define GHOTI_IO_GLTANG_AST_ASTNODE_H

#include <ghoti.io/lang-tang/macros.h>

#ifdef __cplusplus
extern "C" {
#endif //__cplusplus

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <ghoti.io/cutil/float.h>
#include <ghoti.io/cutil/hash.h>
#include <ghoti.io/cutil/string.h>
#include <ghoti.io/cutil/vector.h>
#include <ghoti.io/lang-tang/location.h>
#include <ghoti.io/lang-tang/unicodeString.h>

/**
 * Type prototypes for the node classes.
 */
typedef struct GLTANG_Ast_Node GLTANG_Ast_Node;
typedef struct GLTANG_Ast_Node_Array GLTANG_Ast_Node_Array;
typedef struct GLTANG_Ast_Node_Assign GLTANG_Ast_Node_Assign;
typedef struct GLTANG_Ast_Node_Binary GLTANG_Ast_Node_Binary;
typedef struct GLTANG_Ast_Node_Block GLTANG_Ast_Node_Block;
typedef struct GLTANG_Ast_Node_Boolean GLTANG_Ast_Node_Boolean;
typedef struct GLTANG_Ast_Node_Break GLTANG_Ast_Node_Break;
typedef struct GLTANG_Ast_Node_Cast GLTANG_Ast_Node_Cast;
typedef struct GLTANG_Ast_Node_Continue GLTANG_Ast_Node_Continue;
typedef struct GLTANG_Ast_Node_Do_While GLTANG_Ast_Node_Do_While;
typedef struct GLTANG_Ast_Node_Float GLTANG_Ast_Node_Float;
typedef struct GLTANG_Ast_Node_For GLTANG_Ast_Node_For;
typedef struct GLTANG_Ast_Node_Function GLTANG_Ast_Node_Function;
typedef struct GLTANG_Ast_Node_Function_Call GLTANG_Ast_Node_Function_Call;
typedef struct GLTANG_Ast_Node_Global GLTANG_Ast_Node_Global;
typedef struct GLTANG_Ast_Node_Identifier GLTANG_Ast_Node_Identifier;
typedef struct GLTANG_Ast_Node_If_Else GLTANG_Ast_Node_If_Else;
typedef struct GLTANG_Ast_Node_Index GLTANG_Ast_Node_Index;
typedef struct GLTANG_Ast_Node_Integer GLTANG_Ast_Node_Integer;
typedef struct GLTANG_Ast_Node_Library GLTANG_Ast_Node_Library;
typedef struct GLTANG_Ast_Node_Map GLTANG_Ast_Node_Map;
typedef struct GLTANG_Ast_Node_Map_Pair GLTANG_Ast_Node_Map_Pair;
typedef struct GLTANG_Ast_Node_Parse_Error GLTANG_Ast_Node_Parse_Error;
typedef struct GLTANG_Ast_Node_Period GLTANG_Ast_Node_Period;
typedef struct GLTANG_Ast_Node_Print GLTANG_Ast_Node_Print;
typedef struct GLTANG_Ast_Node_Ranged_For GLTANG_Ast_Node_Ranged_For;
typedef struct GLTANG_Ast_Node_Return GLTANG_Ast_Node_Return;
typedef struct GLTANG_Ast_Node_Slice GLTANG_Ast_Node_Slice;
typedef struct GLTANG_Ast_Node_String GLTANG_Ast_Node_String;
typedef struct GLTANG_Ast_Node_Ternary GLTANG_Ast_Node_Ternary;
typedef struct GLTANG_Ast_Node_Unary GLTANG_Ast_Node_Unary;
typedef struct GLTANG_Ast_Node_Use GLTANG_Ast_Node_Use;
typedef struct GLTANG_Ast_Node_VTable GLTANG_Ast_Node_VTable;
typedef struct GLTANG_Ast_Node_While GLTANG_Ast_Node_While;

// The generated parser header names these types in its value union, and it
// includes this header in turn, so the typedefs must come first.
#include <ghoti.io/lang-tang/ast/tangParser.h>

/**
 * The integer, unsigned integer and float types of the language, and the
 * container types the nodes use for lists.
 *
 * Only 64-bit targets are supported. ctang had a 32-bit branch that never
 * defined the vector type the nodes use, so it could not have built.
 */
#if __SIZEOF_POINTER__ == 8
typedef int64_t GLTANG_Integer;
typedef uint64_t GLTANG_UInteger;
#define GLTANG_INTEGER_MAX INT64_MAX
#define GLTANG_INTEGER_MIN INT64_MIN
#define GLTANG_UINTEGER_MAX UINT64_MAX
typedef GCU_Vector64 GLTANG_VectorX;
#define GLTANG_STRING_HASH gcu_string_hash_64
#define GLTANG_VECTORX_CREATE gcu_vector64_create
#define GLTANG_VECTORX_DESTROY gcu_vector64_destroy
#define GLTANG_VECTORX_APPEND gcu_vector64_append
#define GLTANG_VECTORX_COUNT gcu_vector64_count
#define GLTANG_VECTORX_RESERVE gcu_vector64_reserve
#define GLTANG_TYPEX_P(X) (X).p
#define GLTANG_TYPEX_MAKE_P(X) GCU_TYPE64_P(X)
#else
#error "lang-tang supports 64-bit targets only"
#endif

/**
 * Macros for identifying the AST type.
 *
 * @{
 */
#define GLTANG_AST_IS_ARRAY(X) (((GLTANG_Ast_Node *) X)->vtable == &gltang_ast_node_array_vtable)
#define GLTANG_AST_IS_ASSIGN(X) (((GLTANG_Ast_Node *) X)->vtable == &gltang_ast_node_assign_vtable)
#define GLTANG_AST_IS_BINARY(X) (((GLTANG_Ast_Node *) X)->vtable == &gltang_ast_node_binary_vtable)
#define GLTANG_AST_IS_BLOCK(X) (((GLTANG_Ast_Node *) X)->vtable == &gltang_ast_node_block_vtable)
#define GLTANG_AST_IS_BOOLEAN(X) (((GLTANG_Ast_Node *) X)->vtable == &gltang_ast_node_boolean_vtable)
#define GLTANG_AST_IS_BREAK(X) (((GLTANG_Ast_Node *) X)->vtable == &gltang_ast_node_break_vtable)
#define GLTANG_AST_IS_CAST(X) (((GLTANG_Ast_Node *) X)->vtable == &gltang_ast_node_cast_vtable)
#define GLTANG_AST_IS_CONTINUE(X) (((GLTANG_Ast_Node *) X)->vtable == &gltang_ast_node_continue_vtable)
#define GLTANG_AST_IS_DO_WHILE(X) (((GLTANG_Ast_Node *) X)->vtable == &gltang_ast_node_do_while_vtable)
#define GLTANG_AST_IS_FLOAT(X) (((GLTANG_Ast_Node *) X)->vtable == &gltang_ast_node_float_vtable)
#define GLTANG_AST_IS_FOR(X) (((GLTANG_Ast_Node *) X)->vtable == &gltang_ast_node_for_vtable)
#define GLTANG_AST_IS_FUNCTION(X) (((GLTANG_Ast_Node *) X)->vtable == &gltang_ast_node_function_vtable)
#define GLTANG_AST_IS_FUNCTION_CALL(X) (((GLTANG_Ast_Node *) X)->vtable == &gltang_ast_node_function_call_vtable)
#define GLTANG_AST_IS_GLOBAL(X) (((GLTANG_Ast_Node *) X)->vtable == &gltang_ast_node_global_vtable)
#define GLTANG_AST_IS_IDENTIFIER(X) (((GLTANG_Ast_Node *) X)->vtable == &gltang_ast_node_identifier_vtable)
#define GLTANG_AST_IS_IF_ELSE(X) (((GLTANG_Ast_Node *) X)->vtable == &gltang_ast_node_if_else_vtable)
#define GLTANG_AST_IS_INDEX(X) (((GLTANG_Ast_Node *) X)->vtable == &gltang_ast_node_index_vtable)
#define GLTANG_AST_IS_INTEGER(X) (((GLTANG_Ast_Node *) X)->vtable == &gltang_ast_node_integer_vtable)
#define GLTANG_AST_IS_LIBRARY(X) (((GLTANG_Ast_Node *) X)->vtable == &gltang_ast_node_library_vtable)
#define GLTANG_AST_IS_MAP(X) (((GLTANG_Ast_Node *) X)->vtable == &gltang_ast_node_map_vtable)
#define GLTANG_AST_IS_PARSE_ERROR(X) (((GLTANG_Ast_Node *) X)->vtable == &gltang_ast_node_parse_error_vtable)
#define GLTANG_AST_IS_PERIOD(X) (((GLTANG_Ast_Node *) X)->vtable == &gltang_ast_node_period_vtable)
#define GLTANG_AST_IS_PRINT(X) (((GLTANG_Ast_Node *) X)->vtable == &gltang_ast_node_print_vtable)
#define GLTANG_AST_IS_RANGED_FOR(X) (((GLTANG_Ast_Node *) X)->vtable == &gltang_ast_node_ranged_for_vtable)
#define GLTANG_AST_IS_RETURN(X) (((GLTANG_Ast_Node *) X)->vtable == &gltang_ast_node_return_vtable)
#define GLTANG_AST_IS_SLICE(X) (((GLTANG_Ast_Node *) X)->vtable == &gltang_ast_node_slice_vtable)
#define GLTANG_AST_IS_STRING(X) (((GLTANG_Ast_Node *) X)->vtable == &gltang_ast_node_string_vtable)
#define GLTANG_AST_IS_TERNARY(X) (((GLTANG_Ast_Node *) X)->vtable == &gltang_ast_node_ternary_vtable)
#define GLTANG_AST_IS_UNARY(X) (((GLTANG_Ast_Node *) X)->vtable == &gltang_ast_node_unary_vtable)
#define GLTANG_AST_IS_USE(X) (((GLTANG_Ast_Node *) X)->vtable == &gltang_ast_node_use_vtable)
#define GLTANG_AST_IS_WHILE(X) (((GLTANG_Ast_Node *) X)->vtable == &gltang_ast_node_while_vtable)
#define GLTANG_AST_IS_NUMERIC(X) (GLTANG_AST_IS_INTEGER(X) || GLTANG_AST_IS_FLOAT(X))
#define GLTANG_AST_IS_PRIMITIVE(X) (GLTANG_AST_IS_NUMERIC(X) || GLTANG_AST_IS_STRING(X) || GLTANG_AST_IS_BOOLEAN(X))
/**
 * @}
 */


/**
 * Callback function signature required by the gltang_ast_node_walk() function.
 *
 * @param self The current node being visited.
 * @param data A pointer to the user-defined data that was passed to
 *   gltang_ast_node_walk().
 * @param return_value A pointer to the user-defined return value that was
 *   passed to gltang_ast_node_walk(). This value can be modified by the
 *   callback.
 */
typedef void (*GLTANG_Ast_Node_Walk_Callback)(GLTANG_Ast_Node * self, void * data, void * return_value);

/**
 * The vtable for the GLTANG_Ast_Node class.
 */
struct GLTANG_Ast_Node_VTable {
  /**
   * The name of the class.  It should be unique for each class, and should be
   * suitable for printing in error messages.
   */
  const char * name;
  /**
   * Destroys the node and all of its children.
   *
   * @param self The node to destroy.
   */
  void (*destroy)(GLTANG_Ast_Node * self);
  /**
   * Prints the node and all of its children to stdout.
   *
   * @param self The node to print.
   * @param indent The string to print before each line of output.
   */
  void (*print)(GLTANG_Ast_Node * self, const char * indent);
  /**
   * Generalized function to walk the AST.  The callback function is called for
   * each node in the tree.
   *
   * @param self The node to walk.
   * @param callback A callback function that is called for each node in the
   *   tree.
   * @param data A pointer to user-defined data that is passed to the callback
   *   function.
   * @param return_value A pointer to user-defined return value that is passed to
   *   the callback function.  This value can be modified by the callback.
   */
  void (*walk)(GLTANG_Ast_Node * self, GLTANG_Ast_Node_Walk_Callback callback, void * data, void * return_value);
};

/**
 * The vtable for the GLTANG_Ast_Node class.
 *
 * A basic AST node of this type is used to represent the NULL value.
 */
GLTANG_API_DATA extern GLTANG_Ast_Node_VTable gltang_ast_node_null_vtable;

/**
 * The base class for all AST nodes.
 */
struct GLTANG_Ast_Node {
  /**
   * The vtable for the GLTANG_Ast_Node class.
   */
  GLTANG_Ast_Node_VTable *vtable;
  /**
   * The location of the node in the source code.
   */
  GLTANG_PARSER_LTYPE location;
  /**
   * Whether or not the AST node is a singleton.
   */
  bool is_singleton;
  /**
   * The height of the subtree rooted here: 1 for a leaf, otherwise one more
   * than the tallest child. A constructor that would make a node taller than
   * GLTANG_MAX_TREE_DEPTH refuses instead, so destroy, walk, count, print and
   * the compiler, which recurse once per level, are bounded on every tree
   * that exists.
   */
  uint32_t depth;
};

/**
 * Create a new AST node.
 *
 * @param location The location of the node in the source code.
 * @return The new AST node or NULL on failure.
 */
GLTANG_API GLTANG_NO_DISCARD GLTANG_Ast_Node * gltang_ast_node_create(GLTANG_PARSER_LTYPE location);

/**
 * Destroy the AST node and all of its children.
 *
 * The vtable's destroy function is called to destroy the node.  This function
 * serves as a general dispatch function, and should be used in preference to
 * calling the vtable's destroy function directly.
 *
 * @param self The node to destroy.
 */
GLTANG_API void gltang_ast_node_destroy(GLTANG_Ast_Node * self);

/**
 * Print the AST node and all of its children to stdout.
 *
 * The vtable's print function is called to print the node.  This function serves
 * as a general dispatch function, and should be used in preference to calling
 * the vtable's print function directly.
 *
 * @param self The node to print.
 * @param indent The string to print before each line of output.
 */
GLTANG_API void gltang_ast_node_print(GLTANG_Ast_Node * self, const char * indent);

/**
 * Generalized function to walk the AST.  The callback function is called for
 * each node in the tree.
 *
 * The vtable's walk function is called to walk the node.  This function serves
 * as a general dispatch function, and should be used in preference to calling
 * the vtable's walk function directly.
 *
 * @param self The node to walk.
 * @param callback A callback function that is called for each node in the tree.
 * @param data A pointer to user-defined data that is passed to the callback
 *   function.
 * @param return_value A pointer to user-defined return value that is passed to
 *   the callback function.  This value can be modified by the callback.
 */
GLTANG_API void gltang_ast_node_walk(GLTANG_Ast_Node * self, GLTANG_Ast_Node_Walk_Callback callback, void * data, void * return_value);

/**
 * The destroy function for the GLTANG_Ast_Node class when the node is a null.
 *
 * @param self The node to destroy.
 */
GLTANG_API void gltang_ast_node_null_destroy(GLTANG_Ast_Node * self);

/**
 * The print function for the GLTANG_Ast_Node class when the node is a null.
 *
 * @param self The node to print.
 * @param indent The string to print before each line of output.
 */
GLTANG_API void gltang_ast_node_null_print(GLTANG_Ast_Node * self, const char * indent);

/**
 * The walk function for the GLTANG_Ast_Node class when the node is a null.
 *
 * @param self The node to walk.
 * @param callback A callback function that is called for each node in the tree.
 * @param data A pointer to user-defined data that is passed to the callback
 *   function.
 * @param return_value A pointer to user-defined return value that is passed to
 *   the callback function.  This value can be modified by the callback.
 */
GLTANG_API void gltang_ast_node_null_walk(GLTANG_Ast_Node * self, GLTANG_Ast_Node_Walk_Callback callback, void * data, void * return_value);

#ifdef __cplusplus
}
#endif //__cplusplus

#endif //GHOTI_IO_GLTANG_AST_ASTNODE_H
