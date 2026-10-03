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
 * Declare the GLTANG_Scanner used to tokenize a Tang script.
 */

#ifndef GHOTI_IO_GLTANG_TANGSCANNER_H
#define GHOTI_IO_GLTANG_TANGSCANNER_H

#include <ghoti.io/lang-tang/macros.h>

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

#include <stdbool.h>
#include <stddef.h>

/**
 * The block the scanner allocates from. gltang_parse() reserves it before the
 * scan and releases it after, so a failing allocation is a return value
 * rather than flex's exit(2). See flex/tangScanner.l.
 */
typedef struct GLTANG_Scan_Pool {
  char * base;     ///< The reserved block.
  size_t used;     ///< Bytes handed out, headers included.
  size_t capacity; ///< Size of the block.
  bool out_of_memory; ///< Set by the scanner when a token buffer could not grow.
} GLTANG_Scan_Pool;

/**
 * What the scanner needs beyond a copy of the input: its own state, the
 * buffer state, and the start-condition stack, with their headers. Measured at
 * well under 1 KiB; the margin is for the day a flex release grows its state.
 */
#define GLTANG_SCAN_POOL_SLACK 8192

#define YYSTYPE GLTANG_PARSER_STYPE
#define YYLTYPE GLTANG_PARSER_LTYPE

#include <ghoti.io/lang-tang/ast/tangParser.h>

// Our scanner will populate an internal buffer so that strings can be properly
// interpreted (and null-terminated) by the parser.  The buffer will be passed
// back to the parser as the tokens (IDENTIFIER, STRING, etc.) are recognized.
//
// In the event of a parse error, the parser will need to be able to clean up
// any memory that was allocated for the buffer, hence passing in this pointer
// by reference.
#undef YY_DECL
#define YY_DECL int gltang_scanner_get_next_token(GLTANG_PARSER_STYPE * yylval_param, GLTANG_PARSER_LTYPE * yylloc_param , yyscan_t yyscanner)

// The actual declaration of the scanner function.
YY_DECL;

// #define YY_DECL Tang::TangParser::symbol_type Tang::TangScanner::get_next_token()

// Now, the normal header contents.

// #include <ghoti.io/lang-tang/ast/tangParser.h>


    /**
     * Helper function to set the scanner to template parsing mode.
     */
    // void setModeTemplate() {
      // Formula is taken from the BEGIN macro in tangScanner.cpp.
      // (2) is the value that Flex has assigned the TEMPLATE rule type from
      // tpl.l.
      // this->yy_start = 1 + 2 * (2);
    // }

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // GHOTI_IO_GLTANG_TANGSCANNER_H

