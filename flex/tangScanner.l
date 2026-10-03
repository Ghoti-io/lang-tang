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

/*
 * Generate a Reentrant C Scanner.
 * https://www.cs.virginia.edu/~cr4bd/flex-manual/Code_002dLevel-And-API-Options.html
 * https://www.cs.virginia.edu/~cr4bd/flex-manual/Cxx.html#Cxx
 */
%option reentrant

/*
 * Generate a Bison bridge.
 * https://www.cs.virginia.edu/~cr4bd/flex-manual/Bison-Bridge.html#Bison-Bridge
 */
%option bison-bridge bison-locations

/*
 * Use "gltang_flex_" as a name prefix rather than "yy".
 * https://www.cs.virginia.edu/~cr4bd/flex-manual/Code_002dLevel-And-API-Options.html
 */
%option prefix="gltang_flex"

/*
 * Get rid of yywrap.
 * https://www.cs.virginia.edu/~cr4bd/flex-manual/Options-Affecting-Scanner-Behavior.html
 */
%option noyywrap

/*
 * Cause the scanner to error (rather than supply an empty default rule)
 * when no match is made.  This is handy for finding hols in our rules.
 * https://www.cs.virginia.edu/~cr4bd/flex-manual/Debugging-Options.html
 */
%option nodefault

/*
 * Don't include code for the interactive scanner.  We won't use it.
 * https://www.cs.virginia.edu/~cr4bd/flex-manual/Options-Affecting-Scanner-Behavior.html
 */
%option never-interactive

/*
 * Use a stack to maintain the lexer state.
 * https://www.cs.virginia.edu/~cr4bd/flex-manual/Scanner-Options.html#Scanner-Options
 */
%option stack

/*
 * Take every byte the scanner needs from one block that the caller reserves
 * before the scan starts.
 *
 * flex's own allocations are fatal when they fail: yy_fatal_error() prints a
 * message and calls exit(2), so a scan that could not allocate its buffer
 * ended the host process. ctang inherited that. Here the scanner's allocations
 * (the scanner itself, the input copy, the buffer state and the state stack)
 * are served from a pool that gltang_parse() has already obtained, where
 * failure is an ordinary return value, and the amount is known in advance:
 * the input length plus a fixed slack, since the state stack is nowhere near
 * deep enough to grow.
 */
%option extra-type="GLTANG_Scan_Pool *"
%option noyyalloc noyyrealloc noyyfree

%{
#include <ghoti.io/lang-tang/macros.h>
#include <errno.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <ghoti.io/cutil/memory.h>

#include <ghoti.io/lang-tang/tangScanner.h>
#include <ghoti.io/lang-tang/ast/tangParser.h>
#include <ghoti.io/lang-tang/unicodeString.h>

// A pool allocation that cannot be served means the slack in tangScanner.h is
// wrong for this input. That is the library's own invariant, not an allocation
// failure the caller can recover from, and flex's default (exit(2)) would end
// the host process quietly. Say so and abort, which a test sees.
static void gltang_flex_fatal(const char * message) {
  fprintf(stderr, "lang-tang: scanner invariant failed: %s\n", message);
  abort();
}
#define YY_FATAL_ERROR(msg) gltang_flex_fatal(msg)

// The scanner's own allocation failure reaches the parser as a token it has
// no rule for, so it surfaces as a syntax error. The flag lets gltang_parse()
// tell that from a real one and report ::GLTANG_ERR_OOM.
#define MEMORY_ERROR_TOKEN (yyextra->out_of_memory = true, GLTANG_PARSER_MEMORYERROR)

// yyterminate() is not a function, but a macro.
// The macro does not return the correct type.  It returns an int,
// But the Bison 3 variants requires the return of a Parser::symbol_type.
// Redefine the yyterminate() macro so that it returns the correct type,
// consistent with our use of the Bison variants.
// This macro will be used in Scanner::get_next_token() method (YY_DECL).
// See yyterminate() in http://web.mit.edu/gnu/doc/html/flex_2.html
#define yyterminate() return EOF

// Use the YY_USER_ACTION macro to add some housekeeping code.
// Specifically, we use it to update our location information to keep
// it consistent with the lexer location.
// https://www.cs.virginia.edu/~cr4bd/flex-manual/Misc-Macros.html
// yyleng is the length of the currently matched token.
// yytext is a C-string containing the matched text.
// https://www.cs.virginia.edu/~cr4bd/flex-manual/User-Values.html#User-Values
#define YY_USER_ACTION {                     \
  yylloc->first_line = yylloc->last_line;    \
  yylloc->first_column = yylloc->last_column;\
  for (int i = 0; i < yyleng; ++i) {         \
    if (yytext[i] == '\n') {                 \
      yylloc->last_column = 0;               \
      ++yylloc->last_line;                   \
    }                                        \
    else {                                   \
      ++yylloc->last_column;                 \
    }                                        \
  }                                          \
}

#define CLEANUP_BUFFER        \
  if (stringBuffer) {         \
    gcu_free(stringBuffer);   \
    stringBuffer = 0;         \
    stringBufferLength = 0;   \
    stringBufferReset = true; \
 }


static bool add_to_string(char * * buffer, size_t * bufferLen, char const * append, size_t appendLen, bool * reset) {
  // If reset is true, then we need to clear the buffer and start over.
  if (*reset) {
    *buffer = 0;
    *bufferLen = 0;
    reset = false;
  }

  // If we are starting over, then this is a simple allocation and copy.
  if (!*buffer) {
    // Allocate a new buffer.
    *buffer = gcu_malloc(appendLen + 1);
    if (!*buffer) {
      return false;
    }
    memcpy(*buffer, append, appendLen);
    (*buffer)[appendLen] = 0;
    *bufferLen = appendLen;
    return true;
  }

  // Otherwise, we need to resize the buffer and append the new string.
  void * newPointer = gcu_realloc(*buffer, *bufferLen + appendLen + 1);
  if (newPointer) {
    // The resize succeeded.  Copy the appended string to the original.
    memcpy(&((char *)newPointer)[*bufferLen], append, appendLen);
    ((char *)newPointer)[*bufferLen + appendLen] = 0;
    *buffer = newPointer;
    *bufferLen += appendLen;
    return true;
  }
  return false;
}

static size_t gltang_flex_get_state_stack_depth(void * yyscanner);

%}

/*
 * States used for different parsing modes.  It is useful when needing to
 * create micro-parsers, such as for comments or parsing C-style double-quoted
 * strings.
 * Approach modeled after http://dinosaur.compilertools.net/flex/manpage.html
 *   (search for "C-style")
 * Also given here: https://www.cs.virginia.edu/~cr4bd/flex-manual/Start-Conditions.html
 */
%x SCRIPT TEMPLATE QUICKPRINT doubleQuoteString singleLineComment multiLineComment DATE DATEInsideExpression

%%
  // Include any initialization code here.  These lines *must* be indented,
  // otherwise, flex will try to interpret them as rules rather than code.

  // Holds the contents of a double-quoted string as it is being parsed.
  char * stringBuffer = 0;
  size_t stringBufferLength = 0;
  bool stringBufferReset = true;

  // Identifies the type of string being recognized.
  GLTANG_String_Type type = GLTANG_UNICODE_STRING_TYPE_TRUSTED;

  // Flag variable to track whether or not the EOF has been seen, for use when
  // parsing templates.
  bool eofSeen = false;

<TEMPLATE>{
  "<%=" {
    yy_push_state(QUICKPRINT, yyscanner);
    if (stringBufferLength) {
      yylval->GLTANG_PARSER_QUICKPRINTBEGINANDSTRING = (GLTANG_Parser_Unicode_String){
        .str = stringBuffer,
        .len = stringBufferLength,
        .type = type,
      };
      stringBufferReset = true;
      return GLTANG_PARSER_QUICKPRINTBEGINANDSTRING;
    }
    return GLTANG_PARSER_QUICKPRINTBEGIN;
  }
  "<%" {
    yy_push_state(SCRIPT, yyscanner);
    if (stringBufferLength) {
      yylval->GLTANG_PARSER_TEMPLATESTRING = (GLTANG_Parser_Unicode_String){
        .str = stringBuffer,
        .len = stringBufferLength,
        .type = type,
      };
      stringBufferReset = true;
      return GLTANG_PARSER_TEMPLATESTRING;
    }
  }
  [^<]+ {
    if (!add_to_string(&stringBuffer, &stringBufferLength, yytext, yyleng, &stringBufferReset)) {
      CLEANUP_BUFFER
      return MEMORY_ERROR_TOKEN;
    }
    stringBufferReset = false;
  }
  "<" {
    if (!add_to_string(&stringBuffer, &stringBufferLength, yytext, yyleng, &stringBufferReset)) {
      CLEANUP_BUFFER
      return MEMORY_ERROR_TOKEN;
    }
    stringBufferReset = false;
  }
  <<EOF>> {
    if (!eofSeen && stringBufferLength) {
      eofSeen = true;
      yylval->GLTANG_PARSER_TEMPLATESTRING = (GLTANG_Parser_Unicode_String){
        .str = stringBuffer,
        .len = stringBufferLength,
        .type = type,
      };
      stringBufferReset = true;
      return GLTANG_PARSER_TEMPLATESTRING;
    }
    yyterminate();
  }
}

<INITIAL,SCRIPT,QUICKPRINT,singleLineComment,multiLineComment,DATE,DATEInsideExpression>{
  <<EOF>> {
    yyterminate();
  }
}

<INITIAL,SCRIPT>{
  "%>" {
    // Exit out of SCRIPT mode and return to TEMPLATE mode.
    // Error if we weren't actually in TEMPLATE mode to begin with.
    if (yyg->yy_start_stack_ptr < 1) {
      // Can't pop the stack.  This is an error.
      CLEANUP_BUFFER
      return GLTANG_PARSER_UNEXPECTEDSCRIPTEND;
    }
    yy_pop_state(yyscanner);
    stringBufferReset = true;
  }
}

<QUICKPRINT>{
  "%>" {
    // Exit out of QUICKPRINT mode and return to TEMPLATE mode.
    yy_pop_state(yyscanner);
    stringBufferReset = true;
    return GLTANG_PARSER_QUICKPRINTEND;
  }
}

<INITIAL,SCRIPT,QUICKPRINT>{
  if {
    // " This is to fix the highlighting when editing the flex file.
    return GLTANG_PARSER_IF;
  }
  else {
    return GLTANG_PARSER_ELSE;
  }
  do {
    return GLTANG_PARSER_DO;
  }
  while {
    return GLTANG_PARSER_WHILE;
  }
  for {
    return GLTANG_PARSER_FOR;
  }
  use {
    return GLTANG_PARSER_USE;
  }
  global {
    return GLTANG_PARSER_GLOBAL;
  }
  function {
    return GLTANG_PARSER_FUNCTION;
  }
  return {
    return GLTANG_PARSER_RETURN;
  }
  break {
    return GLTANG_PARSER_BREAK;
  }
  continue {
    return GLTANG_PARSER_CONTINUE;
  }
  \{ {
    return GLTANG_PARSER_LBRACE;
  }
  \} {
    return GLTANG_PARSER_RBRACE;
  }
  as {
    return GLTANG_PARSER_AS;
  }
  ; {
    return GLTANG_PARSER_SEMICOLON;
  }
}

<INITIAL,SCRIPT,QUICKPRINT,DATEInsideExpression>{
  \/\/ {
    // Beginning of a single line comment.
    yy_push_state(singleLineComment, yyscanner);
  }
  \/\* {
    // Beginning of a multi line comment.
    yy_push_state(multiLineComment, yyscanner);
  }
  %\" {
    // " This is to fix the highlighting when editing the flex file.
    // The start of a new, double-quoted string.
    stringBufferReset = true;
    // TODO: This should be a "print" type.
    type = GLTANG_UNICODE_STRING_TYPE_PERCENT;
    yy_push_state(doubleQuoteString, yyscanner);
  }
  \!\" {
    // " This is to fix the highlighting when editing the flex file.
    // The start of a new, double-quoted string.
    stringBufferReset = true;
    // TODO: This should be a "print" type.
    type = GLTANG_UNICODE_STRING_TYPE_HTML;
    yy_push_state(doubleQuoteString, yyscanner);
  }
  \" {
    // " This is to fix the highlighting when editing the flex file.
    // The start of a new, double-quoted string.
    stringBufferReset = true;
    // TODO: This should be a "print" type.
    type = GLTANG_UNICODE_STRING_TYPE_TRUSTED;
    yy_push_state(doubleQuoteString, yyscanner);
  }
  null {
    return GLTANG_PARSER_NULL_;
  }
  int {
    return GLTANG_PARSER_CASTINT;
  }
  float {
    return GLTANG_PARSER_CASTFLOAT;
  }
  bool {
    return GLTANG_PARSER_CASTBOOLEAN;
  }
  string {
    return GLTANG_PARSER_CASTSTRING;
  }
  true {
    yylval->GLTANG_PARSER_BOOLEAN = true;
    return GLTANG_PARSER_BOOLEAN;
  }
  false {
    yylval->GLTANG_PARSER_BOOLEAN = false;
    return GLTANG_PARSER_BOOLEAN;
  }
  print {
    return GLTANG_PARSER_PRINT;
  }
  [0-9]+ {
    // An integer literal that does not fit is rejected, not saturated.
    // strtoll clamps to the maximum and reports it through errno, and that
    // clamp used to be handed back as though the author had written it:
    // `9223372036854775808` became `9223372036854775807`, and because tang has
    // no negative literals, `-9223372036854775808` became
    // `-9223372036854775807` - the wrong number, silently, for the one value
    // the range makes awkward.
    //
    // A literal is the one case where there is nothing to wait for: it has no
    // operands and cannot vary, so the earliest possible report is the right
    // one. That is a parse error rather than a run-time marker.
    errno = 0;
    char * end = 0;
    long long value = strtoll(yytext, &end, 10);
    if ((errno == ERANGE) || (value > (long long)GLTANG_INTEGER_MAX)) {
      // Except for exactly the magnitude of the most negative integer, which
      // is one past the maximum and so is not a literal in its own right - but
      // is the only way to write GLTANG_INTEGER_MIN. It is returned as its own
      // token, which the grammar accepts only directly after unary minus.
      // Java has the same carve-out, for the same reason.
      errno = 0;
      unsigned long long magnitude = strtoull(yytext, 0, 10);
      if ((errno != ERANGE)
        && (magnitude == (unsigned long long)GLTANG_INTEGER_MAX + 1)) {
        yylval->GLTANG_PARSER_INTEGER = GLTANG_INTEGER_MIN;
        return GLTANG_PARSER_INTEGER_MIN_MAGNITUDE;
      }
      return GLTANG_PARSER_INTEGER_OUT_OF_BOUNDS;
    }
    yylval->GLTANG_PARSER_INTEGER = value;
    return GLTANG_PARSER_INTEGER;
  }
  ([0-9]+\.[0-9]*)|(\.[0-9]+) {
    yylval->GLTANG_PARSER_FLOAT = strtold(yytext, 0);
    return GLTANG_PARSER_FLOAT;
  }
  = {
    return GLTANG_PARSER_ASSIGN;
  }
  \+= {
    return GLTANG_PARSER_PLUS_ASSIGN;
  }
  \-= {
    return GLTANG_PARSER_MINUS_ASSIGN;
  }
  \*= {
    return GLTANG_PARSER_MULTIPLY_ASSIGN;
  }
  \/= {
    return GLTANG_PARSER_DIVIDE_ASSIGN;
  }
  %= {
    return GLTANG_PARSER_MODULO_ASSIGN;
  }
  \+ {
    return GLTANG_PARSER_PLUS;
  }
  \- {
    return GLTANG_PARSER_MINUS;
  }
  \* {
    return GLTANG_PARSER_MULTIPLY;
  }
  \/ {
    return GLTANG_PARSER_DIVIDE;
  }
  \% {
    return GLTANG_PARSER_MODULO;
  }
  \( {
    if (gltang_flex_get_state_stack_depth(yyscanner) && (yy_top_state(yyscanner) == DATEInsideExpression)) {
      yy_push_state(DATEInsideExpression, yyscanner);
    }
    return GLTANG_PARSER_LPAREN;
  }
  \) {
    if (gltang_flex_get_state_stack_depth(yyscanner) && (yy_top_state(yyscanner) == DATEInsideExpression)) {
      yy_pop_state(yyscanner);
    }
    return GLTANG_PARSER_RPAREN;
  }
  \[ {
    return GLTANG_PARSER_LBRACKET;
  }
  \] {
    return GLTANG_PARSER_RBRACKET;
  }
  \! {
    return GLTANG_PARSER_EXCLAMATIONPOINT;
  }
  \< {
    return GLTANG_PARSER_LESSTHAN;
  }
  \<= {
    return GLTANG_PARSER_LESSTHANEQUAL;
  }
  \> {
    return GLTANG_PARSER_GREATERTHAN;
  }
  \>= {
    return GLTANG_PARSER_GREATERTHANEQUAL;
  }
  == {
    return GLTANG_PARSER_EQUALCOMPARE;
  }
  != {
    return GLTANG_PARSER_NOTEQUAL;
  }
  && {
    return GLTANG_PARSER_AND;
  }
  \|\| {
    return GLTANG_PARSER_OR;
  }
  \? {
    return GLTANG_PARSER_QUESTIONMARK;
  }
  : {
    return GLTANG_PARSER_COLON;
  }
  , {
    return GLTANG_PARSER_COMMA;
  }
  [a-zA-Z_][a-zA-Z0-9_]* {
    if (!add_to_string(&stringBuffer, &stringBufferLength, yytext, yyleng, &stringBufferReset)) {
      CLEANUP_BUFFER
      return MEMORY_ERROR_TOKEN;
    }
    yylval->GLTANG_PARSER_IDENTIFIER = (GLTANG_Parser_Unicode_String){
      .str = stringBuffer,
      .len = yyleng,
      .type = GLTANG_UNICODE_STRING_TYPE_TRUSTED,
    };
    stringBufferReset = true;
    return GLTANG_PARSER_IDENTIFIER;
  }
  \. {
    return GLTANG_PARSER_PERIOD;
  }
  @ {
    yy_push_state(DATE, yyscanner);
    return GLTANG_PARSER_AT;
  }
  [[:blank:]]+ {
    // Ignore whitespace.
  }
  \n {
    // Ignore newlines.
  }
  . {
    return GLTANG_PARSER_SYNTAXERROR;
  }
}

<DATE>{
  [se][pn]?[yMwdhms] {
    yylval->GLTANG_PARSER_DATE_CLOSE_RELATIVE = (GLTANG_Parser_Date){
      .str = yytext,
      .len = yyleng,
    };
    return GLTANG_PARSER_DATE_CLOSE_RELATIVE;
  }
  now {
    return GLTANG_PARSER_DATE_NOW;
  }
  today {
    return GLTANG_PARSER_DATE_TODAY;
  }
  tomorrow {
    return GLTANG_PARSER_DATE_TOMORROW;
  }
  yesterday {
    return GLTANG_PARSER_DATE_YESTERDAY;
  }
  \+ {
    return GLTANG_PARSER_DATE_PLUS;
  }
  \( {
    yy_push_state(DATEInsideExpression, yyscanner);
  }
  [yMwdhms] {
    yylval->GLTANG_PARSER_DATE_UNIT = (GLTANG_Parser_Date){
      .str = yytext,
      .len = yyleng,
    };
    return GLTANG_PARSER_DATE_UNIT;
  }
  \d+(-\d+(-\d+(T\d+:\d+(:\d+)?)?)?)? {
    yylval->GLTANG_PARSER_DATE_ABSOLUTE = (GLTANG_Parser_Date){
      .str = yytext,
      .len = yyleng,
    };
    return GLTANG_PARSER_DATE_ABSOLUTE;
  }
  Z {
    return GLTANG_PARSER_DATE_TIMEZONE_Z;
  }
  T[+-]?\d+(:\d+(:\d+(.\d+)?)?)? {
    yylval->GLTANG_PARSER_DATE_TIMEZONE_T = (GLTANG_Parser_Date){
      .str = yytext,
      .len = yyleng,
    };
    return GLTANG_PARSER_DATE_TIMEZONE_T;
  }
  (\[[^\]]+\]) {
    yylval->GLTANG_PARSER_DATE_TIMEZONE = (GLTANG_Parser_Date){
      .str = yytext,
      .len = yyleng,
    };
    return GLTANG_PARSER_DATE_TIMEZONE;
  }
  . {
    yy_pop_state(yyscanner);
  }
  \n {
    yy_pop_state(yyscanner);
  }
}

<singleLineComment>{
  \n {
    yy_pop_state(yyscanner);
  }
  .+ {}
}

<multiLineComment>{
  \*\/ {
    yy_pop_state(yyscanner);
  }
  [^*]+ {}
  \* {}
}

<doubleQuoteString>{
  <<EOF>> {
    // CLEANUP_BUFFER, as the other two STRINGERROR returns below do. Input
    // that ends inside a string literal - `x = "abc` - otherwise leaks
    // whatever the literal had accumulated so far, because nothing downstream
    // ever sees this buffer to free it. Found by the parse fuzzer in 30
    // executions.
    CLEANUP_BUFFER
    return GLTANG_PARSER_STRINGERROR;
  }
  \" {
    // " This is to fix the highlighting in Vim.
    // Found the end of a double-quoted string.
    yy_pop_state(yyscanner);
    if (!stringBufferLength) {
      if (!add_to_string(&stringBuffer, &stringBufferLength, "", 0, &stringBufferReset)) {
        CLEANUP_BUFFER
        return MEMORY_ERROR_TOKEN;
      }
    }
    yylval->GLTANG_PARSER_STRING = (GLTANG_Parser_Unicode_String){
      .str = stringBuffer,
      .len = stringBufferLength,
      .type = type,
    };
    stringBufferReset = true;
    return GLTANG_PARSER_STRING;
  }
  \\[0-7]{1,3} {
    // Interpret an escaped octal number.
    // The octal numer is 1-3 digits long, and has a maximum value of 511,
    // which is too large for a single byte.  If this happens, we will
    // generate an error.
    uint32_t result = yytext[1] - '0';
    if (yytext[2]) {
      result <<= 3;
      result += yytext[2] - '0';
      if (yytext[3]) {
        result <<= 3;
        result += yytext[3] - '0';
      }
    }
    if (result > 255) {
      // Generate Error - Bad escape sequence.
      // CLEANUP_BUFFER, as every other error return in this file does. The
      // literal may already have accumulated characters - `"1\421` leaks,
      // `"\421` does not - which is why testing only the shortest case reads
      // as clean. The same shape as the end-of-input leak fixed in b12ebe1.
      CLEANUP_BUFFER
      return GLTANG_PARSER_OCTAL_OUT_OF_BOUNDS;
    }
    char buff[2] = {(unsigned char)result, 0};
    if (!add_to_string(&stringBuffer, &stringBufferLength, buff, 1, &stringBufferReset)) {
      CLEANUP_BUFFER
      return MEMORY_ERROR_TOKEN;
    }
    stringBufferReset = false;
  }
  \\[xX][a-fA-F0-9]{1,2} {
    // Match a hexadecimal char.
    unsigned char result = 0;
    for (int i = 2; i < yyleng; ++i) {
      result <<= 4;
      if (yytext[i] >= 'a' && yytext[i] <= 'f') {
        result += 10 + yytext[i] - 'a';
      }
      else if (yytext[i] >= 'A' && yytext[i] <= 'F') {
        result += 10 + yytext[i] - 'A';
      }
      else {
        result += yytext[i] - '0';
      }
    }
    char buff[2] = {result, 0};

    if (!add_to_string(&stringBuffer, &stringBufferLength, buff, 1, &stringBufferReset)) {
      CLEANUP_BUFFER
      return MEMORY_ERROR_TOKEN;
    }
    stringBufferReset = false;
  }
  \\[0-9]+ {
    // Generate Error - Bad escape sequence.
    // Something like "\48" or "\07777777"
    CLEANUP_BUFFER
    return GLTANG_PARSER_STRINGERROR;
  }
  \n|\\n {
    // Matches either an explicit or escaped newline.
    if (!add_to_string(&stringBuffer, &stringBufferLength, "\n", 1, &stringBufferReset)) {
      CLEANUP_BUFFER
      return MEMORY_ERROR_TOKEN;
    }
    stringBufferReset = false;
  }
  \t|\\t {
    // Matches either an explicit or escaped tab.
    if (!add_to_string(&stringBuffer, &stringBufferLength, "\t", 1, &stringBufferReset)) {
      CLEANUP_BUFFER
      return MEMORY_ERROR_TOKEN;
    }
    stringBufferReset = false;
  }
  \r|\\r {
    // Matches either an explicit or escaped carriage return.
    if (!add_to_string(&stringBuffer, &stringBufferLength, "\r", 1, &stringBufferReset)) {
      CLEANUP_BUFFER
      return MEMORY_ERROR_TOKEN;
    }
    stringBufferReset = false;
  }
  \b|\\b {
    // Matches either an explicit or escaped bell.
    if (!add_to_string(&stringBuffer, &stringBufferLength, "\b", 1, &stringBufferReset)) {
      CLEANUP_BUFFER
      return MEMORY_ERROR_TOKEN;
    }
    stringBufferReset = false;
  }
  \f|\\f {
    // Matches either an explicit or escaped form feed.
    if (!add_to_string(&stringBuffer, &stringBufferLength, "\f", 1, &stringBufferReset)) {
      CLEANUP_BUFFER
      return MEMORY_ERROR_TOKEN;
    }
    stringBufferReset = false;
  }
  \\(.|\n) {
    // Any other escaped character.
    char buff[2] = {yytext[1], 0};
    if (!add_to_string(&stringBuffer, &stringBufferLength, buff, 1, &stringBufferReset)) {
      CLEANUP_BUFFER
      return MEMORY_ERROR_TOKEN;
    }
    stringBufferReset = false;
  }
  [^\\\n\"]+ {
    // "] <-- to fix the highlighting in Vim.
    // Any other character in the string.
    if (!add_to_string(&stringBuffer, &stringBufferLength, yytext, yyleng, &stringBufferReset)) {
      CLEANUP_BUFFER
      return MEMORY_ERROR_TOKEN;
    }
    stringBufferReset = false;
  }
  \\ {
    // Because we specified escape characters, we also need to specify what to
    // do if we see only a single backslash at the end of the input.
    CLEANUP_BUFFER
    return GLTANG_PARSER_STRINGERROR;
  }
}

%%

// Tang helper function to set the initial state to TEMPLATE.
void gltang_flex_set_state(int state, void * yyscanner) {
  yy_push_state(state, yyscanner);
}

// Tang helper function to get the depth of the state stack.
size_t gltang_flex_get_state_stack_depth(void * yyscanner) {
  struct yyguts_t * yyg = (struct yyguts_t*)yyscanner;
	return yyg->yy_start_stack_ptr;
}


// The scanner's allocator: bump allocation from the caller's pool. Every
// block carries its size, so that yyrealloc() can copy, and nothing is
// returned individually; the pool is released whole by its owner.
#define GLTANG_POOL_HEADER 16

void * yyalloc(yy_size_t size, yyscan_t yyscanner) {
  GLTANG_Scan_Pool * pool = yyget_extra(yyscanner);
  if (!pool) {
    return NULL;
  }
  // Round up to the header's alignment. A size near SIZE_MAX would wrap the
  // addition, so it is refused before it is rounded.
  if (size > pool->capacity) {
    return NULL;
  }
  size_t need = (size + (GLTANG_POOL_HEADER - 1)) & ~(size_t)(GLTANG_POOL_HEADER - 1);
  if (need > pool->capacity - pool->used
      || GLTANG_POOL_HEADER > pool->capacity - pool->used - need) {
    return NULL;
  }
  char * block = pool->base + pool->used;
  *(size_t *)(void *)block = size;
  pool->used += GLTANG_POOL_HEADER + need;
  return block + GLTANG_POOL_HEADER;
}

void * yyrealloc(void * pointer, yy_size_t size, yyscan_t yyscanner) {
  void * block = yyalloc(size, yyscanner);
  if (block && pointer) {
    size_t old = *(size_t *)(void *)((char *)pointer - GLTANG_POOL_HEADER);
    memcpy(block, pointer, old < size ? old : size);
  }
  return block;
}

void yyfree(void * pointer, yyscan_t yyscanner) {
  (void)pointer;
  (void)yyscanner;
}
