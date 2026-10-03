// Declarations can be found here:
// https://www.gnu.org/software/bison/manual/html_node/Decl-Summary.html
// Defines can be found here:
// https://www.gnu.org/software/bison/manual/html_node/_0025define-Summary.html

// Minimum version requirement.
// https://www.gnu.org/software/bison/manual/bison.html#index-_0025require
%require "3.8.2"

// Create a .h file with the proper definitions for the lexer.
// https://www.gnu.org/software/bison/manual/bison.html#index-_0025defines-2
%defines

// The generated source includes the generated header by this name, which is
// also the name the installed headers use.
// https://www.gnu.org/software/bison/manual/bison.html#index-_0025define-api_002eheader_002einclude
%define api.header.include {<ghoti.io/lang-tang/ast/tangParser.h>}

// "Namespace" the exported symbols.
// https://www.gnu.org/software/bison/manual/html_node/_0025define-Summary.html#index-_0025define-api_002eprefix
%define api.prefix {GLTANG_Parser_}

// Use union-based values (the default).
// https://www.gnu.org/software/bison/manual/bison.html#index-_0025define-api_002evalue_002etype-union
%define api.value.type union

// Define the location type.
// https://www.gnu.org/software/bison/manual/html_node/Location-Type.html
%define api.location.type {GLTANG_PARSER_LTYPE}

// Use runtime assertions to verify that variant objects are constructed and
// destroyed properly.
// https://www.gnu.org/software/bison/manual/bison.html#index-_0025define-parse_002eassert
%define parse.assert

// Define the YYDEBUG macro so that debugging facilities are compiled.
// https://www.gnu.org/software/bison/manual/bison.html#Tracing
// https://www.gnu.org/software/bison/manual/bison.html#index-_0025define-parse_002etrace
%define parse.trace

// Provide a more helpful error message.
// https://www.gnu.org/software/bison/manual/bison.html#index-_0025define-parse_002eerror-verbose
%define parse.error verbose

// https://www.gnu.org/software/bison/manual/bison.html#Pure-Calling
%define api.pure

// Additional arguments that yylex() should accept.
// https://www.gnu.org/software/bison/manual/bison.html#index-_0025lex_002dparam-3
// YYSTYPE * yylval_param, YYLTYPE * yylloc_param , yyscan_t yyscanner
//%lex-param { YYSTYPE * yyval_param }
//%lex-param { YYLTYPE * yylloc_param }
%lex-param { yyscan_t scanner }

// Additional arguments that yyparse() should accept.
// https://www.gnu.org/software/bison/manual/bison.html#index-_0025parse_002dparam-3
%parse-param { yyscan_t * scanner }
%parse-param { GLTANG_Ast_Node * * ast }
%parse-param { GLTANG_Parser_Error * parseError }
// Where a rule action that rejects its input (invalid UTF-8) says it did, so
// that the error node can name a line and a column. Not in ctang, whose
// parse-error node for such a failure had no location at all.
%parse-param { GLTANG_PARSER_LTYPE * errorLocation }

// Add a prefix to token names
// %define api.token.prefix {TOKEN_}
// Because the prefix will be added to a suffix, do not include any whitespace
// between the prefix and the closing '}'
%define api.token.prefix {GLTANG_PARSER_}

// https://www.gnu.org/software/bison/manual/bison.html#index-_0025token
%token EOF_ 0 "end of code"
%token <int64_t> INTEGER "integer literal"
%token <long double> FLOAT "float literal"
%token <bool> BOOLEAN "boolean literal"
%token <GLTANG_Parser_Unicode_String> STRING "string literal"
%token <GLTANG_Parser_Unicode_String> TEMPLATESTRING "template string"
%token STRINGERROR "Malformed String"
%token <GLTANG_Parser_Unicode_String> IDENTIFIER "identifier"
%token ASSIGN "="
%token PLUS_ASSIGN "+="
%token MINUS_ASSIGN "-="
%token MULTIPLY_ASSIGN "*="
%token DIVIDE_ASSIGN "/="
%token MODULO_ASSIGN "%="
%token PLUS "+"
%token MINUS "-"
%token MULTIPLY "*"
%token DIVIDE "/"
%token MODULO "%"
%token EXCLAMATIONPOINT "!"
%token LPAREN "("
%token RPAREN ")"
%token LESSTHAN "<"
%token LESSTHANEQUAL "<="
%token GREATERTHAN ">"
%token GREATERTHANEQUAL ">="
%token EQUALCOMPARE "=="
%token NOTEQUAL "!="
%token AND "&&"
%token OR "||"
%token LBRACE "{"
%token RBRACE "}"
%token LBRACKET "["
%token RBRACKET "]"
%token IF "if"
%token ELSE "else"
%token DO "do"
%token WHILE "while"
%token FOR "for"
%token AS "as"
%token NULL_ "null"
%token CASTINT "int"
%token CASTFLOAT "float"
%token CASTBOOLEAN "boolean"
%token CASTSTRING "string"
%token USE "use"
%token GLOBAL "global"
%token FUNCTION "function"
%token RETURN "return"
%token BREAK "break"
%token CONTINUE "continue"
%token PRINT "print"
%token QUESTIONMARK "?"
%token COLON ":"
%token SEMICOLON ";"
%token COMMA ","
%token PERIOD "."
%token AT "@"
%token QUICKPRINTBEGIN "<%="
%token <GLTANG_Parser_Unicode_String> QUICKPRINTBEGINANDSTRING "template string followed by <%="
%token QUICKPRINTEND "<%= %> closing tag"
%token UNEXPECTEDSCRIPTEND "%>"
%token MEMORYERROR "Out of Memory/Memory Allocation Error"
%token SYNTAXERROR "Syntax Error"
%token OCTAL_OUT_OF_BOUNDS "Octal literal out of bounds"
%token INTEGER_OUT_OF_BOUNDS "integer literal too large"
// Exactly the magnitude of GLTANG_INTEGER_MIN. Its own token because it is legal
// in one position only - directly after unary minus - and a syntax error
// everywhere else.
%token <int64_t> INTEGER_MIN_MAGNITUDE "integer literal 9223372036854775808"
%token <GLTANG_Parser_Date> DATE_CLOSE_RELATIVE "close, relative date"
%token <GLTANG_Parser_Date> DATE_UNIT "relative date unit (i.e., one of: yMwdhms)"
%token <GLTANG_Parser_Date> DATE_ABSOLUTE "absolute date string"
%token <GLTANG_Parser_Date> DATE_TIMEZONE_T "T timezone specifier with offset"
%token <GLTANG_Parser_Date> DATE_TIMEZONE "timezone descriptor (e.g., [America/Chicago])"
%token DATE_TIMEZONE_Z "Z timezone specifier"
%token DATE_NOW "now date specifier"
%token DATE_TODAY "today date specifier"
%token DATE_YESTERDAY "yesterday date specifier"
%token DATE_TOMORROW "tomorrow date specifier"
%token DATE_PLUS "relative date specifier (i.e., +)"

// Any %type declarations of non-terminals.
// https://www.gnu.org/software/bison/manual/bison.html#index-_0025type
%type <GLTANG_Ast_Node *> program
%type <GLTANG_Ast_Node *> expression
%type <GLTANG_Ast_Node *> libraryExpression
// vector<GLTANG_Ast_Node *>
%type <GCU_Vector64 *> statements
// vector<const char *>
%type <GCU_Vector64 *> functionDeclarationArguments
// vector<GLTANG_Ast_Node *>
%type <GCU_Vector64 *> expressionList
// vector<std::pair<const char *, GLTANG_Ast_Node *>>
%type <GCU_Vector64 *> mapList
%type <GLTANG_Ast_Node *> statement
%type <GLTANG_Ast_Node *> codeBlock
%type <GLTANG_Ast_Node *> openStatement
%type <GLTANG_Ast_Node *> closedStatement
%type <GLTANG_Ast_Node *> optionalExpression
%type <GLTANG_Ast_Node *> slice

// Precedence rules.
// For guidance, see:
// https://efxa.org/2014/05/17/techniques-for-resolving-common-grammar-conflicts-in-parsers/
// Notice that the order is reversed from:
// https://en.cppreference.com/w/cpp/language/operator_precedence
// Here, rules are in order of lowest to highest precedence.
%right "=" "+=" "-=" "*=" "/=" "%=" "?" ":"
%left "||"
%left "&&"
%left "==" "!="
%left "<" "<=" ">" ">="
%left "+" "-"
%left "*" "/" "%"
%right UMINUS AS "!"
%left "(" ")" "[" "]" "."

// Destructors allow us to clean up memory when an error is encountered.
// https://www.gnu.org/software/bison/manual/html_node/Destructor-Decl.html
%destructor {
  gcu_free((void *)$$.str);
} IDENTIFIER STRING TEMPLATESTRING QUICKPRINTBEGINANDSTRING
// Both guard against a null value. A rule action that detects an error assigns
// $$ = 0 and breaks, and error recovery then discards that symbol and runs its
// destructor - so a destructor that cannot accept null turns a handled error
// into an abort. gltang_ast_node_destroy asserts on null, and this is a build
// with asserts live.
%destructor {
  if ($$) {
    gcu_vector64_destroy($$);
  }
} statements functionDeclarationArguments expressionList mapList
%destructor {
  if ($$) {
    gltang_ast_node_destroy($$);
  }
} expression libraryExpression statement codeBlock openStatement closedStatement optionalExpression slice


// Code sections.
// https://www.gnu.org/software/bison/manual/bison.html#g_t_0025code-Summary
// `requires` will be included in the .h file.
%code requires {
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

// Generated from bison/tangParser.y by the Makefile. Do not edit.
#include <stdint.h>
#include <ghoti.io/cutil/hash.h>
#include <ghoti.io/lang-tang/macros.h>
#include <ghoti.io/lang-tang/ast/astNode.h>
#include <ghoti.io/lang-tang/location.h>
#include <ghoti.io/lang-tang/unicodeString.h>

/* An opaque pointer. */
#ifndef YY_TYPEDEF_YY_SCANNER_T
#define YY_TYPEDEF_YY_SCANNER_T
typedef void* yyscan_t;
#endif

#define YY_DECL int gltang_scanner_get_next_token(YYSTYPE * yylval, YYLTYPE * yylloc, yyscan_t yyscanner)

typedef const char * GLTANG_Parser_Error;

/** Which kind of failure a GLTANG_Parser_Error stands for. */
typedef enum GLTANG_Parser_Error_Kind {
  GLTANG_PARSER_ERROR_KIND_NONE,
  GLTANG_PARSER_ERROR_KIND_OUT_OF_MEMORY,
  GLTANG_PARSER_ERROR_KIND_SYNTAX,
  GLTANG_PARSER_ERROR_KIND_INVALID_UTF8,
} GLTANG_Parser_Error_Kind;

/**
 * For "string" expressions, we need to store the string type and the string
 */
typedef struct GLTANG_Parser_Unicode_String {
  const char * str;
  size_t len;
  GLTANG_String_Type type;
} GLTANG_Parser_Unicode_String;

/**
 * For "date" expressions, with the information available in the script.
 */
typedef struct GLTANG_Parser_Date {
  const char * str;
  size_t len;
} GLTANG_Parser_Date;

}

// `top` will be included at the top of the .c file, but not in .h.
%code top {
#include <ghoti.io/lang-tang/macros.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>
#include <ghoti.io/cutil/memory.h>
#include <ghoti.io/unicode/utf.h>
#include <ghoti.io/lang-tang/ast/tangParser.h>
#include <ghoti.io/lang-tang/tangScanner.h>
#include <ghoti.io/lang-tang/ast/astNodeAll.h>

static GLTANG_Parser_Error ErrorOutOfMemory = "Out of memory/Memory allocation error";

// Every value *parseError can hold must outlive the parse. These literals do.
// bison's own message does not - yymsg points into a buffer freed when yyparse
// returns - so GLTANG_Parser_error stores this instead and puts the detailed
// message, which is copied, into the parse-error node.
static GLTANG_Parser_Error ErrorSyntax = "Syntax error";

// A string token that is not valid UTF-8 is the author's input, not a failure
// of the machine. ctang reported it as "Out of memory", because both come out
// of the same failed string creation; here the two are told apart before the
// token is released, so that a caller (and the host's error message) can say
// which it was. The accepted language is unchanged: both are refusals.
static GLTANG_Parser_Error ErrorInvalidUtf8 = "Invalid UTF-8 in a string";
// static GLTANG_Parser_Error ErrorOctalOutOfBounds = true;
// static GLTANG_Parser_Error ErrorStringError = true;
// static GLTANG_Parser_Error ErrorUnexpectedScriptEnd = true;

void GLTANG_Parser_error(GLTANG_PARSER_LTYPE * yylloc, yyscan_t * scanner, GLTANG_Ast_Node * * ast, GLTANG_Parser_Error * parseError, GLTANG_PARSER_LTYPE * errorLocation, const char * yymsg);

// We must provide the yylex() function.
// yylex() arguments are defined in the bison .y file.
// It is conceivable that a programmer may want to have multiple compilers in
// the same project.  Each compiler will need its own yylex() function.
// Because yylex() is only defined and used in this file, we can set its
// linkage as "internal", by declaring the function "static", which allows
// each compiler's yylex() to not interfere with that of another linked file.
// https://en.cppreference.com/w/cpp/language/storage_duration
static int GLTANG_Parser_lex(GLTANG_PARSER_STYPE * yylval_param, GLTANG_PARSER_LTYPE * yylloc_param , yyscan_t yyscanner) {
  return gltang_scanner_get_next_token(yylval_param, yylloc_param, yyscanner);
}


// Helper cleanup function for vector64 of ast nodes.
static void vector64_ast_node_cleanup(GCU_Vector64 * vector) {
  for (size_t i = 0; i < vector->count; ++i) {
    gltang_ast_node_destroy((GLTANG_Ast_Node *)vector->data[i].p);
  }
}

// Helper cleanup function for vector64 of map pairs.
static void vector64_map_pair_cleanup(GCU_Vector64 * vector) {
  for (size_t i = 0; i < vector->count; ++i) {
    gltang_ast_node_destroy((GLTANG_Ast_Node *)((GLTANG_Ast_Node_Map_Pair *)vector->data[i].p)->key);
    gltang_ast_node_destroy((GLTANG_Ast_Node *)((GLTANG_Ast_Node_Map_Pair *)vector->data[i].p)->value);
    gcu_free((void *)vector->data[i].p);
  }
}

// `a += b` is parsed into the tree for `a = a + b`, so the arithmetic, the
// overflow reporting and the string concatenation all come from the operators
// that already exist and are already tested - and both engines get it without
// knowing the spelling was different.
//
// The target identifier is therefore needed in two places, as the thing
// assigned to and as the left operand of the arithmetic. An identifier node
// owns its name, so the name is duplicated; sharing one node between the two
// positions would free it twice.
//
// Only an identifier can be the target. `a[i] += b` would need a deep copy of
// the index expression, which does not exist, and desugaring it without one
// would evaluate `i` twice - so `a[f()] += 1` would call f() twice. A compound
// assignment that silently calls a function twice is worse than not having one,
// so the grammar refuses the form instead.
//
// Takes ownership of name.str and of rhs on every path, including failure.
static GLTANG_Ast_Node * make_compound_assign(GLTANG_Parser_Unicode_String name, GLTANG_Ast_Node * rhs, GLTANG_Binary_Type operator_type, GLTANG_PARSER_LTYPE location) {
  char * duplicate = gcu_malloc(name.len + 1);
  if (!duplicate) {
    gcu_free((void *)name.str);
    gltang_ast_node_destroy(rhs);
    return NULL;
  }
  memcpy(duplicate, name.str, name.len);
  duplicate[name.len] = '\0';

  // The read copy adopts the token's own string, the write copy the duplicate.
  GLTANG_Ast_Node * read = (GLTANG_Ast_Node *)gltang_ast_node_identifier_create(name.str, location);
  if (!read) {
    gcu_free((void *)name.str);
    gcu_free(duplicate);
    gltang_ast_node_destroy(rhs);
    return NULL;
  }

  GLTANG_Ast_Node * target = (GLTANG_Ast_Node *)gltang_ast_node_identifier_create(duplicate, location);
  if (!target) {
    gcu_free(duplicate);
    gltang_ast_node_destroy(read);
    gltang_ast_node_destroy(rhs);
    return NULL;
  }

  GLTANG_Ast_Node * operation = (GLTANG_Ast_Node *)gltang_ast_node_binary_create(read, rhs, operator_type, location);
  if (!operation) {
    gltang_ast_node_destroy(read);
    gltang_ast_node_destroy(rhs);
    gltang_ast_node_destroy(target);
    return NULL;
  }

  // gltang_ast_node_binary_create owns read and rhs from here, so the failure
  // path below destroys the operation rather than its parts.
  GLTANG_Ast_Node * assignment = (GLTANG_Ast_Node *)gltang_ast_node_assign_create(target, operation, location);
  if (!assignment) {
    gltang_ast_node_destroy(operation);
    gltang_ast_node_destroy(target);
    return NULL;
  }
  return assignment;
}


#define LOCATION(A, B)              \
  GLTANG_PARSER_LTYPE location = {     \
    .first_line = A.first_line,     \
    .first_column = A.first_column, \
    .last_line = B.last_line,       \
    .last_column = B.last_column,   \
  }


// Discarding a semantic value that a failing rule will not use.
//
// bison will not do this for us. Its %destructor runs for symbols discarded
// during error recovery, but by the time a rule action executes, the RHS has
// already been popped off the value stack into $1..$n - from bison's point of
// view the reduction consumed them. So a rule that detects an error and bails
// is the last owner of everything it was handed, and dropping $$ on the floor
// leaks all of it. `s = "\xc8"` leaked the identifier node and its buffer
// exactly this way.
//
// _Generic rather than one macro per type combination: the VERIFY macros are
// used with nodes, vectors and token strings in every mix, and naming the
// type at 67 call sites is how the two lists drift apart. A type that turns up
// here without a case below is a compile error, which is the right outcome -
// it means a new kind of value needs a decision about who frees it.
static inline void gltang_parser_discard_node(GLTANG_Ast_Node * value) {
  if (value) {
    gltang_ast_node_destroy(value);
  }
}

static inline void gltang_parser_discard_vector(GCU_Vector64 * value) {
  if (value) {
    gcu_vector64_destroy(value);
  }
}

static inline void gltang_parser_discard_token_string(GLTANG_Parser_Unicode_String value) {
  // The scanner hands ownership of .str to the parser with the token.
  gcu_free((void *)value.str);
}

static inline void gltang_parser_discard_nothing_i(int64_t value) { (void)value; }
static inline void gltang_parser_discard_nothing_f(long double value) { (void)value; }
static inline void gltang_parser_discard_nothing_b(bool value) { (void)value; }

// Whether a semantic value is "missing". Pointer values carry failure as null;
// a token struct cannot be null and never signals failure this way, so it
// always answers false. This exists so VERIFY1..4 accept every value type -
// `!A` does not compile for a struct, which is why arms holding only a token
// used the zero-argument VERIFY, and that is exactly the form that forgets to
// free what it is discarding.
static inline bool gltang_parser_missing_node(GLTANG_Ast_Node * value) { return !value; }
static inline bool gltang_parser_missing_vector(GCU_Vector64 * value) { return !value; }
static inline bool gltang_parser_missing_token(GLTANG_Parser_Unicode_String value) { (void)value; return false; }
static inline bool gltang_parser_missing_i(int64_t value) { (void)value; return false; }
static inline bool gltang_parser_missing_f(long double value) { (void)value; return false; }
static inline bool gltang_parser_missing_b(bool value) { (void)value; return false; }

#define MISSING(X) _Generic((X),                                  \
  GLTANG_Ast_Node *: gltang_parser_missing_node,                        \
  GCU_Vector64 *: gltang_parser_missing_vector,                      \
  GLTANG_Parser_Unicode_String: gltang_parser_missing_token,            \
  int64_t: gltang_parser_missing_i,                                  \
  long double: gltang_parser_missing_f,                              \
  bool: gltang_parser_missing_b                                      \
)(X)

#define DISCARD(X) _Generic((X),                                  \
  GLTANG_Ast_Node *: gltang_parser_discard_node,                        \
  GCU_Vector64 *: gltang_parser_discard_vector,                      \
  GLTANG_Parser_Unicode_String: gltang_parser_discard_token_string,     \
  int64_t: gltang_parser_discard_nothing_i,                          \
  long double: gltang_parser_discard_nothing_f,                      \
  bool: gltang_parser_discard_nothing_b                              \
)(X)

// Records why a string token could not be made into a string. Must run before
// the token's buffer is released, since it reads that buffer.
#define STRING_FAILURE(TOKEN, LOC)                                      \
  if (guni_utf8_validate((TOKEN).str, (TOKEN).len, NULL) != GUNI_OK) { \
    *parseError = ErrorInvalidUtf8;                                     \
    *errorLocation = (LOC);                                             \
  }                                                                     \
  else {                                                                \
    *parseError = ErrorOutOfMemory;                                     \
  }

#define VERIFY(Z)   \
  if (*parseError) { \
    Z = 0;          \
    break;          \
  }

#define VERIFY1(A,Z)      \
  if (*parseError || MISSING(A)) { \
    DISCARD(A);           \
    Z = 0;                \
    break;                \
  }

#define VERIFY2(A,B,Z)          \
  if (*parseError || MISSING(A) || MISSING(B)) { \
    DISCARD(A);                 \
    DISCARD(B);                 \
    Z = 0;                      \
    break;                      \
  }

#define VERIFY3(A,B,C,Z)              \
  if (*parseError || MISSING(A) || MISSING(B) || MISSING(C)) { \
    DISCARD(A);                       \
    DISCARD(B);                       \
    DISCARD(C);                       \
    Z = 0;                            \
    break;                            \
  }

#define VERIFY4(A,B,C,D,Z)                  \
  if (*parseError || MISSING(A) || MISSING(B) || MISSING(C) || MISSING(D)) { \
    DISCARD(A);                             \
    DISCARD(B);                             \
    DISCARD(C);                             \
    DISCARD(D);                             \
    Z = 0;                                  \
    break;                                  \
  }

#define BINARY_TEMPLATE(X,A,AA,B,BB,Z)     \
  VERIFY2(A,B,Z);                          \
  LOCATION(AA, BB);                        \
  Z = (GLTANG_Ast_Node *)gltang_ast_node_binary_create(A, B, X, location); \
  if (!Z) {                                \
    DISCARD(A);                            \
    DISCARD(B);                            \
    *parseError = ErrorOutOfMemory;        \
    break;                                 \
  }

#define UNARY_TEMPLATE(X,AA,B,BB,Z)        \
  VERIFY1(B,Z);                            \
  LOCATION(AA, BB);                        \
  Z = (GLTANG_Ast_Node *)gltang_ast_node_unary_create(B, X, location); \
  if (!Z) {                                \
    DISCARD(B);                            \
    *parseError = ErrorOutOfMemory;        \
  }

#define CAST_TEMPLATE(X,A,AA,BB,Z)         \
  VERIFY1(A,Z);                            \
  LOCATION(AA, BB);                        \
  Z = (GLTANG_Ast_Node *)gltang_ast_node_cast_create(A, X, location); \
  if (!Z) {                                \
    DISCARD(A);                            \
    *parseError = ErrorOutOfMemory;        \
    break;                                 \
  }


// End of Code top section.
}
// The grammar start symbol (non-terminal).
// https://www.gnu.org/software/bison/manual/bison.html#index-_0025start
%start program


%%
// Grammar Section.
// https://efxa.org/2014/05/17/techniques-for-resolving-common-grammar-conflicts-in-parsers/
// https://stackoverflow.com/a/12732388/3821565

// `program` represents every possible syntactically-valid program.
program
  : expression
    {
      // VERIFY1 as every other rule does. Without it this took $1 even when
      // the expression's action had bailed without assigning $$, so *ast
      // became whatever the value union happened to hold - for a STRING that
      // is the token's .str, which the same action had just freed. Three
      // bytes of input reached a use-after-free that way.
      VERIFY1($1, *ast);

      *ast = (GLTANG_Ast_Node *)$1;
    }
  | statements
    {
      // Verify that there have been no memory errors.
      VERIFY1($1,$$);

      *ast = (GLTANG_Ast_Node *)gltang_ast_node_block_create($1, @1);
      if (!*ast) {
        DISCARD($1);
        *parseError = ErrorOutOfMemory;
      }
    }
  | EOF_
    {}
  ;

// `functionDeclarationArguments` is a comma-separated list of variable names
// given as part of a function declaration.
functionDeclarationArguments
  : %empty
    {
      // Verify that there have been no memory errors.
      VERIFY($$);

      $$ = gcu_vector64_create(0);
      if (!$$) {
        *parseError = ErrorOutOfMemory;
        break;
      }
      $$->cleanup = vector64_ast_node_cleanup;
    }
  | IDENTIFIER
    {
      // Verify that there have been no memory errors.
      VERIFY1($1,$$);

      // Create the identifier object.
      const char * identifier = $1.str;
      GLTANG_Ast_Node * parameter = (GLTANG_Ast_Node *)gltang_ast_node_identifier_create(identifier, @1);
      if (!parameter) {
        gcu_free((void *)identifier);
        *parseError = ErrorOutOfMemory;
        // $$ = 0 before breaking: bison initialises $$ to $1, and $1 here is
        // not a value of this rule's type, so leaving it makes the cleanup
        // path destroy the wrong thing. See the STRING arm for the full note.
        $$ = 0;
        break;
      }

      // Create a vector to hold the identifiers.
      $$ = gcu_vector64_create(1);
      if (!$$) {
        gltang_ast_node_destroy(parameter);
        *parseError = ErrorOutOfMemory;
        break;
      }
      $$->cleanup = vector64_ast_node_cleanup;

      gcu_vector64_append($$, GCU_TYPE64_P(parameter));
    }
  | functionDeclarationArguments "," IDENTIFIER
    {
      // Verify that there have been no memory errors.
      VERIFY2($1,$3,$$);

      const char * identifier = $3.str;
      GLTANG_Ast_Node * parameter = (GLTANG_Ast_Node *)gltang_ast_node_identifier_create(identifier, @3);
      if (!parameter) {
        gcu_free((void *)identifier);
        *parseError = ErrorOutOfMemory;
        break;
      }

      if (!gcu_vector64_append($1, GCU_TYPE64_P(parameter))) {
        gltang_ast_node_destroy(parameter);
        DISCARD($1);
        $$ = 0;
        *parseError = ErrorOutOfMemory;
        break;
      }
      $$ = $1;
    }
  ;

// `expressionList` is a comma-separated list of expressions given as part
// of a function call or an array declaration.
expressionList
  : %empty
    {
      // Verify that there have been no memory errors.
      VERIFY($$);

      $$ = gcu_vector64_create(0);
      if (!$$) {
        *parseError = ErrorOutOfMemory;
        break;
      }
      $$->cleanup = vector64_ast_node_cleanup;
    }
  | expression
    {
      // Verify that there have been no memory errors.
      VERIFY1($1,$$);

      $$ = gcu_vector64_create(1);
      if (!$$) {
        DISCARD($1);
        *parseError = ErrorOutOfMemory;
        break;
      }
      $$->cleanup = vector64_ast_node_cleanup;
      gcu_vector64_append($$, GCU_TYPE64_P($1));
    }
  | expressionList "," expression
    {
      // Verify that there have been no memory errors.
      VERIFY2($1,$3,$$);

      if (!gcu_vector64_append($1, GCU_TYPE64_P($3))) {
        DISCARD($1);
        DISCARD($3);
        $$ = 0;
        *parseError = ErrorOutOfMemory;
        break;
      }
      $$ = $1;
    }
  ;

// `mapList` is a comma-separated list of expressions given as part of a
// map declaration.
mapList
  : IDENTIFIER ":" expression
    {
      // Verify that there have been no memory errors.
      VERIFY2($1,$3,$$);

      GLTANG_Unicode_String * key_unicode_string = gltang_unicode_string_create_and_adopt($1.str, $1.len, $1.type);
      if (!key_unicode_string) {
        DISCARD($3);
        *parseError = ErrorOutOfMemory;
        gcu_free((void *)$1.str);
        // $$ = 0 before breaking: bison initialises $$ to $1, and $1 here is
        // not a value of this rule's type, so leaving it makes the cleanup
        // path destroy the wrong thing. See the STRING arm for the full note.
        $$ = 0;
        break;
      }

      GLTANG_Ast_Node * key = (GLTANG_Ast_Node *)gltang_ast_node_string_create(key_unicode_string, @1);
      if (!key) {
        // Destroying key_unicode_string frees $1.str, which it adopted.
        gltang_unicode_string_destroy(key_unicode_string);
        DISCARD($3);
        *parseError = ErrorOutOfMemory;
        $$ = 0;
        break;
      }

      // Base case.  Create a vector to hold additional entries.
      $$ = gcu_vector64_create(32);
      if (!$$) {
        gltang_ast_node_destroy(key);
        DISCARD($3);
        *parseError = ErrorOutOfMemory;
        break;
      }
      $$->cleanup = vector64_map_pair_cleanup;

      // Create the pair.
      GLTANG_Ast_Node_Map_Pair * pair = gcu_malloc(sizeof(GLTANG_Ast_Node_Map_Pair));
      if (!pair) {
        gltang_ast_node_destroy(key);
        DISCARD($3);
        gcu_vector64_destroy($$);
        *parseError = ErrorOutOfMemory;
        $$ = 0;
        break;
      }

      // Populate all the info for this pair.
      pair->key = key;
      pair->value = $3;
      if (!gcu_vector64_append($$, GCU_TYPE64_P((void *)pair))) {
        gcu_free(pair);
        gltang_ast_node_destroy(key);
        DISCARD($3);
        gcu_vector64_destroy($$);
        *parseError = ErrorOutOfMemory;
        $$ = 0;
        break;
      }
    }
  | mapList "," IDENTIFIER ":" expression
    {
      // Verify that there have been no memory errors.
      VERIFY3($1,$3,$5,$$)

      GLTANG_Unicode_String * key_unicode_string = gltang_unicode_string_create_and_adopt($3.str, $3.len, $3.type);
      if (!key_unicode_string) {
        DISCARD($5);
        *parseError = ErrorOutOfMemory;
        gcu_free((void *)$3.str);
        break;
      }

      GLTANG_Ast_Node * key = (GLTANG_Ast_Node *)gltang_ast_node_string_create(key_unicode_string, @3);
      if (!key) {
        gltang_unicode_string_destroy(key_unicode_string);
        DISCARD($5);
        *parseError = ErrorOutOfMemory;
        break;
      }

      // Create the pair.
      GLTANG_Ast_Node_Map_Pair * pair = gcu_malloc(sizeof(GLTANG_Ast_Node_Map_Pair));
      if (!pair) {
        gltang_ast_node_destroy(key);
        DISCARD($1);
        DISCARD($5);
        *parseError = ErrorOutOfMemory;
        $$ = 0;
        break;
      }

      // Populate all the info for this pair.
      pair->key = key;
      pair->value = $5;
      if (!gcu_vector64_append($1, GCU_TYPE64_P(pair))) {
        gltang_ast_node_destroy(key);
        gcu_free(pair);
        DISCARD($1);
        DISCARD($5);
        *parseError = ErrorOutOfMemory;
        $$ = 0;
        break;
      }

      $$ = $1;
    }
  | mapList ","
  ;

// `statements` represent a sequence of `statement` expressions.
statements
  : statement
    {
      // Verify that there have been no memory errors.
      VERIFY1($1,$$);

      // Base case.  Create a vector to hold additional entries.
      $$ = gcu_vector64_create(1);
      if (!$$) {
        DISCARD($1);
        *parseError = ErrorOutOfMemory;
        break;
      }
      $$->cleanup = vector64_ast_node_cleanup;

      // Space already reserved.
      gcu_vector64_append($$, GCU_TYPE64_P($1));
    }
  | statements statement
    {
      // Verify that there have been no memory errors.
      VERIFY2($1,$2,$$)

      if (!gcu_vector64_append($1, GCU_TYPE64_P($2))) {
        DISCARD($1);
        DISCARD($2);
        *parseError = ErrorOutOfMemory;
        $$ = 0;
        break;
      }
      $$ = $1;
    }
  ;

// `statement` represents an `expression`
statement
  : closedStatement
  | openStatement
  ;

// To avoid the "dangling else" problem:
// https://en.wikipedia.org/wiki/Dangling_else#Avoiding_the_conflict_in_LR_parsers
// These should only contain closedStatements

closedStatement
  : "if" "(" expression ")" closedStatement "else" closedStatement
    {
      // Verify that there have been no memory errors.
      VERIFY3($3,$5,$7,$$);

      LOCATION(@1, @7);
      $$ = (GLTANG_Ast_Node *)gltang_ast_node_if_else_create($3, $5, $7, location);
      if (!$$) {
        DISCARD($3);
        DISCARD($5);
        DISCARD($7);
        *parseError = ErrorOutOfMemory;
        break;
      }
    }
  | "while" "(" expression ")" closedStatement
    {
      // Verify that there have been no memory errors.
      VERIFY2($3,$5,$$);

      LOCATION(@1, @5);
      $$ = (GLTANG_Ast_Node *)gltang_ast_node_while_create($3, $5, location);
      if (!$$) {
        DISCARD($3);
        DISCARD($5);
        *parseError = ErrorOutOfMemory;
        break;
      }
    }
  | "do" statement "while" "(" expression ")" ";"
    {
      // Verify that there have been no memory errors.
      VERIFY2($2,$5,$$);

      LOCATION(@1, @7);
      $$ = (GLTANG_Ast_Node *)gltang_ast_node_do_while_create($5, $2, location);
      if (!$$) {
        DISCARD($5);
        DISCARD($2);
        *parseError = ErrorOutOfMemory;
        break;
      }
    }
  | "for" "(" optionalExpression ";" optionalExpression ";" optionalExpression ")" closedStatement
    {
      // Verify that there have been no memory errors.
      VERIFY4($3,$5,$7,$9,$$);

      LOCATION(@1, @9);
      $$ = (GLTANG_Ast_Node *)gltang_ast_node_for_create($3, $5, $7, $9, location);
      if (!$$) {
        DISCARD($3);
        DISCARD($5);
        DISCARD($7);
        DISCARD($9);
        *parseError = ErrorOutOfMemory;
        break;
      }
    }
  | "for" "(" IDENTIFIER ":" expression ")" closedStatement
    {
      // Verify that there have been no memory errors.
      VERIFY3($3,$5,$7,$$);

      const char * identifier = $3.str;

      LOCATION(@1, @7);
      $$ = (GLTANG_Ast_Node *)gltang_ast_node_ranged_for_create(identifier, $5, $7, location);
      if (!$$) {
        DISCARD($5);
        DISCARD($7);
        gcu_free((void *)identifier);
        *parseError = ErrorOutOfMemory;
        break;
      }
    }
  | "function" IDENTIFIER "(" functionDeclarationArguments ")" codeBlock
    {
      // Verify that there have been no memory errors.
      VERIFY3($2,$4,$6,$$);

      const char * identifier = $2.str;

      LOCATION(@1, @6);
      $$ = (GLTANG_Ast_Node *)gltang_ast_node_function_create(identifier, $4, $6, location);
      if (!$$) {
        DISCARD($4);
        DISCARD($6);
        gcu_free((void *)identifier);
        *parseError = ErrorOutOfMemory;
        break;
      }
    }
  | codeBlock
  | "return" ";"
    {
      // Verify that there have been no memory errors.
      VERIFY($$);

      $$ = (GLTANG_Ast_Node *)gltang_ast_node_return_create(0, @1);
      if (!$$) {
        *parseError = ErrorOutOfMemory;
        break;
      }
    }
  | "return" expression ";"
    {
      // Verify that there have been no memory errors.
      VERIFY1($2,$$);

      LOCATION(@1, @3);
      $$ = (GLTANG_Ast_Node *)gltang_ast_node_return_create($2, location);
      if (!$$) {
        DISCARD($2);
        *parseError = ErrorOutOfMemory;
        break;
      }
    }
  | "break" ";"
    {
      // Verify that there have been no memory errors.
      VERIFY($$);

      $$ = (GLTANG_Ast_Node *)gltang_ast_node_break_create(@1);
      if (!$$) {
        *parseError = ErrorOutOfMemory;
        break;
      }
    }
  | "continue" ";"
    {
      // Verify that there have been no memory errors.
      VERIFY($$);

      $$ = (GLTANG_Ast_Node *)gltang_ast_node_continue_create(@1);
      if (!$$) {
        *parseError = ErrorOutOfMemory;
        break;
      }
    }
  | expression ";"
  | TEMPLATESTRING
    {
      // Verify that there have been no memory errors.
      VERIFY1($1,$$);

      GLTANG_Unicode_String * string = gltang_unicode_string_create_and_adopt($1.str, $1.len, $1.type);
      if (!string) {
        // $$ must be cleared before breaking, for the same reason as the
        // STRING arm below: leaving it unset leaves the value union holding
        // $1, whose first member is the .str freed on the next line, and every
        // consumer then treats that freed pointer as a GLTANG_Ast_Node *. This
        // arm is reached for invalid UTF-8, not only for allocation failure,
        // so it is ordinary input - a single byte 0xC7 as a template was a
        // use-after-free.
        $$ = 0;
        STRING_FAILURE($1, @1)
        gcu_free((void *)$1.str);
        break;
      }

      GLTANG_Ast_Node * template_string = (GLTANG_Ast_Node *)gltang_ast_node_string_create(string, @1);
      // template_string, not $$: $$ still holds the token here, so testing it
      // asked whether the input had a string rather than whether the node was
      // allocated. The guard could never fire, and a real failure fell through
      // into gltang_ast_node_print_create(NULL).
      if (!template_string) {
        gltang_unicode_string_destroy(string);
        *parseError = ErrorOutOfMemory;
        $$ = 0;
        break;
      }

      GLTANG_Ast_Node * print_template_string = (GLTANG_Ast_Node *)gltang_ast_node_print_create(template_string, @1);
      if (!print_template_string) {
        gltang_ast_node_destroy(template_string);
        $$ = 0;
        *parseError = ErrorOutOfMemory;
        break;
      }

      $$ = print_template_string;
    }
  | QUICKPRINTBEGINANDSTRING expression QUICKPRINTEND
    {
      // This rule is for when a template string is followed by a quick print.
      // Verify that there have been no memory errors.
      VERIFY2($1,$2,$$);

      LOCATION(@1, @3);

      // This arm owns two things it was handed: $1.str and $2. Every exit
      // before $2 is adopted below has to free both - the RHS has already
      // been popped, so no %destructor will run for them. Freeing only the
      // string leaked the whole parsed expression, and the first branch is
      // reached by ordinary input: gltang_unicode_string_create_and_adopt fails
      // on invalid UTF-8, so `\x87<%=t%>` leaked the identifier node.
      GLTANG_Unicode_String * string = gltang_unicode_string_create_and_adopt($1.str, $1.len, $1.type);
      if (!string) {
        STRING_FAILURE($1, @1)
        gcu_free((void *)$1.str);
        gltang_ast_node_destroy($2);
        $$ = 0;
        break;
      }

      GLTANG_Ast_Node * preceding = (GLTANG_Ast_Node *)gltang_ast_node_string_create(string, @1);
      if (!preceding) {
        gltang_unicode_string_destroy(string);
        gltang_ast_node_destroy($2);
        $$ = 0;
        *parseError = ErrorOutOfMemory;
        break;
      }

      GLTANG_Ast_Node * print_preceding = (GLTANG_Ast_Node *)gltang_ast_node_print_create(preceding, @1);
      if (!print_preceding) {
        gltang_ast_node_destroy(preceding);
        gltang_ast_node_destroy($2);
        $$ = 0;
        *parseError = ErrorOutOfMemory;
        break;
      }

      // gltang_ast_node_print_create only adopts $2 when it succeeds.
      GLTANG_Ast_Node * print_expression = (GLTANG_Ast_Node *)gltang_ast_node_print_create($2, @2);
      if (!print_expression) {
        gltang_ast_node_destroy(print_preceding);
        gltang_ast_node_destroy($2);
        $$ = 0;
        *parseError = ErrorOutOfMemory;
        break;
      }

      GCU_Vector64 * block = gcu_vector64_create(2);
      if (!block) {
        gltang_ast_node_destroy(print_preceding);
        gltang_ast_node_destroy(print_expression);
        $$ = 0;
        *parseError = ErrorOutOfMemory;
        break;
      }
      block->cleanup = vector64_ast_node_cleanup;
      gcu_vector64_append(block, GCU_TYPE64_P((void *)print_preceding));
      gcu_vector64_append(block, GCU_TYPE64_P((void *)print_expression));

      $$ = (GLTANG_Ast_Node *)gltang_ast_node_block_create(block, location);
      if (!$$) {
        gcu_vector64_destroy(block);
        *parseError = ErrorOutOfMemory;
        break;
      }
    }
  | QUICKPRINTBEGIN expression QUICKPRINTEND
    {
      // This rule is for when a quick print is not preceded by a template string.
      // Verify that there have been no memory errors.
      VERIFY1($2,$$);

      LOCATION(@1, @3);
      $$ = (GLTANG_Ast_Node *)gltang_ast_node_print_create($2, location);
      if (!$$) {
        // print_create only adopts $2 when it succeeds.
        gltang_ast_node_destroy($2);
        *parseError = ErrorOutOfMemory;
        break;
      }
    }
  | "use" IDENTIFIER ";"
    {
      // Verify that there have been no memory errors.
      VERIFY1($2,$$);

      const char * identifier = $2.str;

      LOCATION(@1, @3);
      GLTANG_Ast_Node * library = (GLTANG_Ast_Node *)gltang_ast_node_library_create(identifier, location);
      if (!library) {
        gcu_free((void *)identifier);
        *parseError = ErrorOutOfMemory;
        // $$ = 0 before breaking: bison initialises $$ to $1, and $1 here is
        // not a value of this rule's type, so leaving it makes the cleanup
        // path destroy the wrong thing. See the STRING arm for the full note.
        $$ = 0;
        break;
      }
      // Copy the identifier.
      const char * identifier_copy = gcu_calloc(strlen(identifier) + 1, sizeof(char));
      if (!identifier_copy) {
        // The library node adopted `identifier`, so destroying it is what
        // frees it. ctang freed it here as well, and then destroyed the
        // library: a double free on an allocation failure.
        gltang_ast_node_destroy(library);
        *parseError = ErrorOutOfMemory;
        $$ = 0;
        break;
      }
      strcpy((char *)identifier_copy, identifier);
      $$ = (GLTANG_Ast_Node *)gltang_ast_node_use_create(identifier_copy, library, location);
      if (!$$) {
        // The copy was never adopted either; ctang leaked it.
        gcu_free((void *)identifier_copy);
        gltang_ast_node_destroy(library);
        *parseError = ErrorOutOfMemory;
        break;
      }
    }
  | "use" libraryExpression "as" IDENTIFIER ";"
    {
      // Verify that there have been no memory errors.
      VERIFY2($2,$4,$$);

      const char * identifier = $4.str;

      LOCATION(@1, @5);
      $$ = (GLTANG_Ast_Node *)gltang_ast_node_use_create(identifier, $2, location);
      if (!$$) {
        DISCARD($2);
        gcu_free((void *)identifier);
        *parseError = ErrorOutOfMemory;
      }
    }
  | "global" IDENTIFIER ";"
    {
      // Verify that there have been no memory errors.
      VERIFY1($2,$$);

      const char * identifier = $2.str;

      GLTANG_Ast_Node * name = (GLTANG_Ast_Node *)gltang_ast_node_identifier_create(identifier, @2);
      if (!name) {
        gcu_free((void *)identifier);
        *parseError = ErrorOutOfMemory;
        // $$ = 0 before breaking: bison initialises $$ to $1, and $1 here is
        // not a value of this rule's type, so leaving it makes the cleanup
        // path destroy the wrong thing. See the STRING arm for the full note.
        $$ = 0;
        break;
      }

      LOCATION(@1, @3);
      $$ = (GLTANG_Ast_Node *)gltang_ast_node_global_create(name, 0, location);
      if (!$$) {
        gltang_ast_node_destroy(name);
        *parseError = ErrorOutOfMemory;
        break;
      }
    }
    | "global" IDENTIFIER "=" expression ";"
    {
      // Verify that there have been no memory errors. The IDENTIFIER is named
      // too: an earlier error discards what this rule was handed, and ctang
      // named only the expression, leaking the identifier's buffer.
      VERIFY2($2,$4,$$);

      const char * identifier = $2.str;

      GLTANG_Ast_Node * name = (GLTANG_Ast_Node *)gltang_ast_node_identifier_create(identifier, @2);
      if (!name) {
        gcu_free((void *)identifier);
        DISCARD($4);
        *parseError = ErrorOutOfMemory;
        // $$ = 0 before breaking: bison initialises $$ to $1, and $1 here is
        // not a value of this rule's type, so leaving it makes the cleanup
        // path destroy the wrong thing. See the STRING arm for the full note.
        $$ = 0;
        break;
      }

      LOCATION(@1, @5);
      $$ = (GLTANG_Ast_Node *)gltang_ast_node_global_create(name, $4, location);
      if (!$$) {
        DISCARD($4);
        gltang_ast_node_destroy(name);
        *parseError = ErrorOutOfMemory;
        break;
      }
    }
  ;

libraryExpression
  : IDENTIFIER
    {
      // Verify that there have been no memory errors.
      VERIFY1($1,$$);

      const char * identifier = $1.str;

      $$ = (GLTANG_Ast_Node *)gltang_ast_node_library_create(identifier, @1);
      if (!$$) {
        gcu_free((void *)identifier);
        *parseError = ErrorOutOfMemory;
      }
    }
  | libraryExpression "." IDENTIFIER
    {
      // Verify that there have been no memory errors.
      VERIFY2($1,$3,$$);

      const char * identifier = $3.str;

      LOCATION(@1, @3);
      $$ = (GLTANG_Ast_Node *)gltang_ast_node_period_create($1, identifier, location);
      if (!$$) {
        DISCARD($1);
        gcu_free((void *)identifier);
        *parseError = ErrorOutOfMemory;
      }
    }
  | libraryExpression "." "global"
    {
      // Verify that there have been no memory errors.
      VERIFY1($1,$$);

      char * identifier = gcu_calloc(7, sizeof(char));
      if (!identifier) {
        *parseError = ErrorOutOfMemory;
        break;
      }
      strcpy(identifier, "global");

      LOCATION(@1, @3);
      $$ = (GLTANG_Ast_Node *)gltang_ast_node_period_create($1, identifier, location);
      if (!$$) {
        DISCARD($1);
        gcu_free(identifier);
        *parseError = ErrorOutOfMemory;
      }
    }
  ;

// These should only have an openStatement as the last terminal.
openStatement
  : "if" "(" expression ")" statement
    {
      // Verify that there have been no memory errors.
      VERIFY2($3,$5,$$);

      LOCATION(@1, @5);
      $$ = (GLTANG_Ast_Node *)gltang_ast_node_if_else_create($3, $5, 0, location);
      if (!$$) {
        DISCARD($3);
        DISCARD($5);
        *parseError = ErrorOutOfMemory;
        break;
      }
    }
  | "if" "(" expression ")" closedStatement "else" openStatement
    {
      // Verify that there have been no memory errors.
      VERIFY3($3,$5,$7,$$);

      LOCATION(@1, @7);
      $$ = (GLTANG_Ast_Node *)gltang_ast_node_if_else_create($3, $5, $7, location);
      if (!$$) {
        DISCARD($3);
        DISCARD($5);
        DISCARD($7);
        *parseError = ErrorOutOfMemory;
        break;
      }
    }
  | "while" "(" expression ")" openStatement
    {
      // Verify that there have been no memory errors.
      VERIFY2($3,$5,$$);

      LOCATION(@1, @5);
      $$ = (GLTANG_Ast_Node *)gltang_ast_node_while_create($3, $5, location);
      if (!$$) {
        DISCARD($3);
        DISCARD($5);
        *parseError = ErrorOutOfMemory;
        break;
      }
    }
  | "for" "(" optionalExpression ";" optionalExpression ";" optionalExpression ")" openStatement
    {
      // Verify that there have been no memory errors.
      VERIFY4($3,$5,$7,$9,$$);

      LOCATION(@1, @9);
      $$ = (GLTANG_Ast_Node *)gltang_ast_node_for_create($3, $5, $7, $9, location);
      if (!$$) {
        DISCARD($3);
        DISCARD($5);
        DISCARD($7);
        DISCARD($9);
        *parseError = ErrorOutOfMemory;
        break;
      }
    }
  | "for" "(" IDENTIFIER ":" expression ")" openStatement
    {
      // Verify that there have been no memory errors.
      VERIFY3($3,$5,$7,$$);

      // Copy the identifier.
      const char * identifier = $3.str;

      LOCATION(@1, @7);
      $$ = (GLTANG_Ast_Node *)gltang_ast_node_ranged_for_create(identifier, $5, $7, location);
      if (!$$) {
        DISCARD($5);
        DISCARD($7);
        gcu_free((void *)identifier);
        *parseError = ErrorOutOfMemory;
        break;
      }
    }
  ;

// `optionalExpression` is an expression that, if not present, will default to
// a null value.
optionalExpression
  : %empty
    {
      // Verify that there have been no memory errors.
      VERIFY($$);

      GLTANG_PARSER_LTYPE location = {
        .first_line = 0,
        .first_column = 0,
        .last_line = 0,
        .last_column = 0,
      };
      $$ = gltang_ast_node_create(location);
      if (!$$) {
        *parseError = ErrorOutOfMemory;
      }
    }
  | expression
  ;

// `slice` represents a slice operation on a container.
slice
  : expression "[" optionalExpression ":" optionalExpression ":" optionalExpression "]"
    {
      // Verify that there have been no memory errors.
      VERIFY4($1,$3,$5,$7,$$);

      LOCATION(@1, @8);
      $$ = (GLTANG_Ast_Node *)gltang_ast_node_slice_create($1, $3, $5, $7, location);
      if (!$$) {
        DISCARD($1);
        DISCARD($3);
        DISCARD($5);
        DISCARD($7);
        *parseError = ErrorOutOfMemory;
        break;
      }
    }
  | expression "[" optionalExpression ":" optionalExpression "]"
    {
      // Verify that there have been no memory errors.
      VERIFY3($1,$3,$5,$$);

      LOCATION(@1, @6);
      $$ = (GLTANG_Ast_Node *)gltang_ast_node_slice_create($1, $3, $5, 0, location);
      if (!$$) {
        DISCARD($1);
        DISCARD($3);
        DISCARD($5);
        *parseError = ErrorOutOfMemory;
        break;
      }
    }
  ;

// `codeBlock` represents a series of statements.
codeBlock
  : "{" "}"
    {
      // Verify that there have been no memory errors.
      VERIFY($$);

      LOCATION(@1, @2);
      GLTANG_Ast_Node * null_val = gltang_ast_node_create(location);
      if (!null_val) {
        *parseError = ErrorOutOfMemory;
        // $$ = 0 before breaking: bison initialises $$ to $1, and $1 here is
        // not a value of this rule's type, so leaving it makes the cleanup
        // path destroy the wrong thing. See the STRING arm for the full note.
        $$ = 0;
        break;
      }
      GCU_Vector64 * vector = gcu_vector64_create(1);
      if (!vector) {
        gltang_ast_node_destroy(null_val);
        *parseError = ErrorOutOfMemory;
        $$ = 0;
        break;
      }
      vector->cleanup = vector64_ast_node_cleanup;
      gcu_vector64_append(vector, GCU_TYPE64_P((void *)null_val));
      $$ = (GLTANG_Ast_Node *)gltang_ast_node_block_create(vector, location);
      if (!$$) {
        gcu_vector64_destroy(vector);
        *parseError = ErrorOutOfMemory;
      }
    }
  | "{" statements "}"
    {
      // Verify that there have been no memory errors.
      VERIFY1($2,$$);

      LOCATION(@1, @3);
      $$ = (GLTANG_Ast_Node *)gltang_ast_node_block_create($2, location);
      if (!$$) {
        DISCARD($2);
        *parseError = ErrorOutOfMemory;
        break;
      }
    }
  ;

// `expression` represents a computable value.
expression
  : NULL_
    {
      // Verify that there have been no memory errors.
      VERIFY($$);

      $$ = (GLTANG_Ast_Node *)gltang_ast_node_create(@1);
      if (!$$) {
        *parseError = ErrorOutOfMemory;
        break;
      }
    }
  | IDENTIFIER
    {
      // Verify that there have been no memory errors.
      VERIFY1($1,$$);

      // Copy the identifier.
      const char * identifier = $1.str;

      $$ = (GLTANG_Ast_Node *)gltang_ast_node_identifier_create(identifier, @1);
      if (!$$) {
        gcu_free((void *)identifier);
        *parseError = ErrorOutOfMemory;
        break;
      }
    }
  | INTEGER
    {
      // Verify that there have been no memory errors.
      VERIFY($$);

      $$ = (GLTANG_Ast_Node *)gltang_ast_node_integer_create($1, @1);
      if (!$$) {
        DISCARD($1);
        *parseError = ErrorOutOfMemory;
        break;
      }
    }
  | FLOAT
    {
      // Verify that there have been no memory errors.
      VERIFY($$);

      $$ = (GLTANG_Ast_Node *)gltang_ast_node_float_create($1, @1);
      if (!$$) {
        DISCARD($1);
        *parseError = ErrorOutOfMemory;
        break;
      }
    }
  | BOOLEAN
    {
      // Verify that there have been no memory errors.
      VERIFY($$);

      $$ = (GLTANG_Ast_Node *)gltang_ast_node_boolean_create($1, @1);
      if (!$$) {
        DISCARD($1);
        *parseError = ErrorOutOfMemory;
      }
    }
  | STRING
    {
      // Verify that there have been no memory errors.
      VERIFY1($1,$$);

      GLTANG_Unicode_String * string = gltang_unicode_string_create_and_adopt((const char * const)$1.str, $1.len, $1.type);
      if (!string) {
        // $$ must be cleared before breaking. Leaving it unset leaves the
        // value union holding $1, whose first member is the .str freed on the
        // next line, and every consumer of this rule then treats that freed
        // pointer as a GLTANG_Ast_Node *. Note this arm is reached for invalid
        // UTF-8, not only for allocation failure, so it is ordinary input.
        $$ = 0;
        STRING_FAILURE($1, @1)
        gcu_free((void *)$1.str);
        break;
      }

      $$ = (GLTANG_Ast_Node *)gltang_ast_node_string_create(string, @1);
      if (!$$) {
        gltang_unicode_string_destroy(string);
        *parseError = ErrorOutOfMemory;
      }
    }
  | expression "=" expression
    {
      // Verify that there have been no memory errors.
      VERIFY2($1,$3,$$);

      LOCATION(@1, @3);
      $$ = (GLTANG_Ast_Node *)gltang_ast_node_assign_create($1, $3, location);
      if (!$$) {
        DISCARD($1);
        DISCARD($3);
        *parseError = ErrorOutOfMemory;
      }
    }
  | IDENTIFIER "+=" expression
    {
      // Verify that there have been no memory errors.
      // VERIFY2 rather than VERIFY1: $1 is the identifier token and owns its
      // string, so discarding only $3 on the error path leaks the name.
      VERIFY2($1,$3,$$);

      LOCATION(@1, @3);
      // make_compound_assign frees everything it was handed if it fails, and
      // returns NULL - which is already what $$ must be - so there is nothing
      // to undo here. Matches the plain assignment arm above.
      $$ = make_compound_assign($1, $3, GLTANG_BINARY_TYPE_ADD, location);
      if (!$$) {
        *parseError = ErrorOutOfMemory;
      }
    }
  | IDENTIFIER "-=" expression
    {
      // Verify that there have been no memory errors.
      // VERIFY2 rather than VERIFY1: $1 is the identifier token and owns its
      // string, so discarding only $3 on the error path leaks the name.
      VERIFY2($1,$3,$$);

      LOCATION(@1, @3);
      // make_compound_assign frees everything it was handed if it fails, and
      // returns NULL - which is already what $$ must be - so there is nothing
      // to undo here. Matches the plain assignment arm above.
      $$ = make_compound_assign($1, $3, GLTANG_BINARY_TYPE_SUBTRACT, location);
      if (!$$) {
        *parseError = ErrorOutOfMemory;
      }
    }
  | IDENTIFIER "*=" expression
    {
      // Verify that there have been no memory errors.
      // VERIFY2 rather than VERIFY1: $1 is the identifier token and owns its
      // string, so discarding only $3 on the error path leaks the name.
      VERIFY2($1,$3,$$);

      LOCATION(@1, @3);
      // make_compound_assign frees everything it was handed if it fails, and
      // returns NULL - which is already what $$ must be - so there is nothing
      // to undo here. Matches the plain assignment arm above.
      $$ = make_compound_assign($1, $3, GLTANG_BINARY_TYPE_MULTIPLY, location);
      if (!$$) {
        *parseError = ErrorOutOfMemory;
      }
    }
  | IDENTIFIER "/=" expression
    {
      // Verify that there have been no memory errors.
      // VERIFY2 rather than VERIFY1: $1 is the identifier token and owns its
      // string, so discarding only $3 on the error path leaks the name.
      VERIFY2($1,$3,$$);

      LOCATION(@1, @3);
      // make_compound_assign frees everything it was handed if it fails, and
      // returns NULL - which is already what $$ must be - so there is nothing
      // to undo here. Matches the plain assignment arm above.
      $$ = make_compound_assign($1, $3, GLTANG_BINARY_TYPE_DIVIDE, location);
      if (!$$) {
        *parseError = ErrorOutOfMemory;
      }
    }
  | IDENTIFIER "%=" expression
    {
      // Verify that there have been no memory errors.
      // VERIFY2 rather than VERIFY1: $1 is the identifier token and owns its
      // string, so discarding only $3 on the error path leaks the name.
      VERIFY2($1,$3,$$);

      LOCATION(@1, @3);
      // make_compound_assign frees everything it was handed if it fails, and
      // returns NULL - which is already what $$ must be - so there is nothing
      // to undo here. Matches the plain assignment arm above.
      $$ = make_compound_assign($1, $3, GLTANG_BINARY_TYPE_MODULO, location);
      if (!$$) {
        *parseError = ErrorOutOfMemory;
      }
    }
  | expression "+" expression
    {
      BINARY_TEMPLATE(GLTANG_BINARY_TYPE_ADD,$1,@1,$3,@3,$$);
    }
  | expression "-" expression
    {
      BINARY_TEMPLATE(GLTANG_BINARY_TYPE_SUBTRACT,$1,@1,$3,@3,$$);
    }
  | expression "*" expression
    {
      BINARY_TEMPLATE(GLTANG_BINARY_TYPE_MULTIPLY,$1,@1,$3,@3,$$);
    }
  | expression "/" expression
    {
      BINARY_TEMPLATE(GLTANG_BINARY_TYPE_DIVIDE,$1,@1,$3,@3,$$);
    }
  | expression "%" expression
    {
      BINARY_TEMPLATE(GLTANG_BINARY_TYPE_MODULO,$1,@1,$3,@3,$$);
    }
  | "-" expression %prec UMINUS
    {
      UNARY_TEMPLATE(GLTANG_UNARY_TYPE_NEGATIVE,@1,$2,@2,$$);
    }
  | "-" INTEGER_MIN_MAGNITUDE %prec UMINUS
    {
      // Verify that there have been no memory errors.
      VERIFY($$);

      // The magnitude of the most negative integer is one past the maximum, so
      // it is not a valid literal by itself and the scanner refuses it
      // everywhere else. It is accepted here because tang has no negative
      // literals - unary minus is an operator - so this is the only way to
      // write GLTANG_INTEGER_MIN at all. The value is produced whole rather than
      // by negating, which would itself overflow.
      LOCATION(@1, @2);
      $$ = (GLTANG_Ast_Node *)gltang_ast_node_integer_create($2, location);
      if (!$$) {
        DISCARD($2);
        *parseError = ErrorOutOfMemory;
        break;
      }
    }
  | "!" expression
    {
      UNARY_TEMPLATE(GLTANG_UNARY_TYPE_NOT,@1,$2,@2,$$);
    }
  | expression "<" expression
    {
      BINARY_TEMPLATE(GLTANG_BINARY_TYPE_LESS_THAN,$1,@1,$3,@3,$$);
    }
  | expression "<=" expression
    {
      BINARY_TEMPLATE(GLTANG_BINARY_TYPE_LESS_THAN_EQUAL,$1,@1,$3,@3,$$);
    }
  | expression ">" expression
    {
      BINARY_TEMPLATE(GLTANG_BINARY_TYPE_GREATER_THAN,$1,@1,$3,@3,$$);
    }
  | expression ">=" expression
    {
      BINARY_TEMPLATE(GLTANG_BINARY_TYPE_GREATER_THAN_EQUAL,$1,@1,$3,@3,$$);
    }
  | expression "==" expression
    {
      BINARY_TEMPLATE(GLTANG_BINARY_TYPE_EQUAL,$1,@1,$3,@3,$$);
    }
  | expression "!=" expression
    {
      BINARY_TEMPLATE(GLTANG_BINARY_TYPE_NOT_EQUAL,$1,@1,$3,@3,$$);
    }
  | expression "&&" expression
    {
      BINARY_TEMPLATE(GLTANG_BINARY_TYPE_AND,$1,@1,$3,@3,$$);
    }
  | expression "||" expression
    {
      BINARY_TEMPLATE(GLTANG_BINARY_TYPE_OR,$1,@1,$3,@3,$$);
    }
  | slice
  | "(" expression ")"
    {
      // Verify that there have been no memory errors.
      VERIFY1($2,$$);

      $$ = $2;
    }
  | expression "as" "int"
    {
      CAST_TEMPLATE(GLTANG_CAST_TYPE_INTEGER,$1,@1,@3,$$);
    }
  | expression "as" "float"
    {
      CAST_TEMPLATE(GLTANG_CAST_TYPE_FLOAT,$1,@1,@3,$$);
    }
  | expression "as" "boolean"
    {
      CAST_TEMPLATE(GLTANG_CAST_TYPE_BOOLEAN,$1,@1,@3,$$);
    }
  | expression "as" "string"
    {
      CAST_TEMPLATE(GLTANG_CAST_TYPE_STRING,$1,@1,@3,$$);
    }
  | "print" "(" expression ")"
    {
      // Verify that there have been no memory errors.
      VERIFY1($3,$$);

      LOCATION(@1, @4);
      $$ = (GLTANG_Ast_Node *)gltang_ast_node_print_create($3, location);
      if (!$$) {
        DISCARD($3);
        *parseError = ErrorOutOfMemory;
      }
    }
  | expression "." IDENTIFIER
    {
      // Verify that there have been no memory errors.
      VERIFY2($1,$3,$$);

      const char * identifier = $3.str;

      LOCATION(@1, @3);
      $$ = (GLTANG_Ast_Node *)gltang_ast_node_period_create($1, identifier, location);
      if (!$$) {
        DISCARD($1);
        gcu_free((void *)identifier);
        *parseError = ErrorOutOfMemory;
      }
    }
  | expression "." "global"
    {
      // Verify that there have been no memory errors.
      VERIFY1($1,$$);

      char * identifier = gcu_calloc(7, sizeof(char));
      if (!identifier) {
        *parseError = ErrorOutOfMemory;
        break;
      }
      strcpy(identifier, "global");

      LOCATION(@1, @3);
      $$ = (GLTANG_Ast_Node *)gltang_ast_node_period_create($1, identifier, location);
      if (!$$) {
        DISCARD($1);
        gcu_free(identifier);
        *parseError = ErrorOutOfMemory;
      }
    }
  |  "[" expressionList "]"
    {
      // Verify that there have been no memory errors.
      VERIFY1($2,$$);

      LOCATION(@1, @3);
      $$ = (GLTANG_Ast_Node *)gltang_ast_node_array_create($2, location);
      if (!$$) {
        DISCARD($2);
        *parseError = ErrorOutOfMemory;
        break;
      }
    }
  | "{" ":" "}"
    {
      // Verify that there have been no memory errors.
      VERIFY($$);

      LOCATION(@1, @3);
      GCU_Vector64 * vector = gcu_vector64_create(0);
      if (!vector) {
        *parseError = ErrorOutOfMemory;
        // $$ = 0 before breaking: bison initialises $$ to $1, and $1 here is
        // not a value of this rule's type, so leaving it makes the cleanup
        // path destroy the wrong thing. See the STRING arm for the full note.
        $$ = 0;
        break;
      }
      vector->cleanup = vector64_map_pair_cleanup;
      $$ = (GLTANG_Ast_Node *)gltang_ast_node_map_create(vector, location);
      if (!$$) {
        gcu_vector64_destroy(vector);
        *parseError = ErrorOutOfMemory;
      }
    }
  | "{" mapList "}"
    {
      // Verify that there have been no memory errors.
      VERIFY1($2,$$);

      LOCATION(@1, @3);
      $$ = (GLTANG_Ast_Node *)gltang_ast_node_map_create($2, location);
      if (!$$) {
        DISCARD($2);
        *parseError = ErrorOutOfMemory;
      }
    }
  | expression "[" expression "]"
    {
      // Verify that there have been no memory errors.
      VERIFY2($1,$3,$$);

      LOCATION(@1, @4);
      $$ = (GLTANG_Ast_Node *)gltang_ast_node_index_create($1, $3, location);
      if (!$$) {
        DISCARD($1);
        DISCARD($3);
        *parseError = ErrorOutOfMemory;
        break;
      }
    }
  | expression "(" expressionList ")"
    {
      // Verify that there have been no memory errors.
      VERIFY2($1,$3,$$);

      LOCATION(@1, @4);
      $$ = (GLTANG_Ast_Node *)gltang_ast_node_function_call_create($1, $3, location);
      if (!$$) {
        DISCARD($1);
        DISCARD($3);
        *parseError = ErrorOutOfMemory;
        break;
      }
    }
  | expression "?" expression ":" expression
    {
      // Verify that there have been no memory errors.
      VERIFY3($1,$3,$5,$$);

      LOCATION(@1, @5);
      $$ = (GLTANG_Ast_Node *)gltang_ast_node_ternary_create($1, $3, $5, location);
      if (!$$) {
        DISCARD($1);
        DISCARD($3);
        DISCARD($5);
        *parseError = ErrorOutOfMemory;
        break;
      }
    }
  ;

%%

// https://www.gnu.org/software/bison/manual/bison.html#YYERROR
void GLTANG_Parser_error(GLTANG_PARSER_LTYPE * yylloc, GLTANG_MAYBE_UNUSED(yyscan_t * scanner), GLTANG_Ast_Node * * ast, GLTANG_Parser_Error * parseError, GLTANG_MAYBE_UNUSED(GLTANG_PARSER_LTYPE * errorLocation), const char * yymsg) {
  // A stable literal, not yymsg: see ErrorSyntax above. The detailed message
  // goes into the node below, which copies it.
  *parseError = ErrorSyntax;

  // Always record the failure as a node, not only when a partial AST happens
  // to exist. Most syntax errors are caught before the start rule has assigned
  // anything, so the old `if (*ast)` meant no node was built, the parse
  // returned null - which already means "there was nothing to parse", an empty
  // source being valid - and gltang_program_create turned that into an empty
  // program. A script with a syntax error therefore compiled, ran, printed
  // nothing and reported success.
  //
  // The node copies the message, which matters: yymsg points into a buffer
  // bison frees when yyparse returns, so it cannot outlive the parse. That is
  // also why *parseError above is only ever compared against null by the
  // rules, and must not be read by a caller.
  if (*ast) {
    gltang_ast_node_destroy(*ast);
  }
  *ast = (GLTANG_Ast_Node *)gltang_ast_node_parse_error_create(yymsg, *yylloc);
}


// Which of this file's error literals a parse stopped with. The rule actions
// store them as pointers to private strings, so the caller cannot compare.
GLTANG_Parser_Error_Kind gltang_parser_error_kind(GLTANG_Parser_Error error) {
  if (error == ErrorOutOfMemory) {
    return GLTANG_PARSER_ERROR_KIND_OUT_OF_MEMORY;
  }
  if (error == ErrorSyntax) {
    return GLTANG_PARSER_ERROR_KIND_SYNTAX;
  }
  if (error == ErrorInvalidUtf8) {
    return GLTANG_PARSER_ERROR_KIND_INVALID_UTF8;
  }
  return GLTANG_PARSER_ERROR_KIND_NONE;
}
