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
 *
 * The one program in this repository that includes ctang (AD-2, AD-16).
 *
 * Usage: oracle_ctang <script|template|run-script|run-template> <file>
 *
 * The parse modes (`script`, `template`) parse the file with frozen ctang's own
 * parse entry points and print one line: `ok <node-count>` when it accepts the
 * source (an empty source is `ok 0`, as ctang's NULL meant "nothing to
 * parse"), or `error` when it refuses.
 *
 * The run modes (`run-script`, `run-template`) execute the file through
 * ctang's own interpreter: `gta_program_create_with_flags` and
 * `gta_program_execute` with `GTA_PROGRAM_FLAG_DISABLE_BINARY` (and the
 * environment ignored), so ctang runs the program in its bytecode virtual
 * machine and never in the x86-64 JIT. ctang has exactly those two executors
 * and requires them to agree (language reference section 1); the JIT is left
 * out because it exists on one architecture only and asserts while compiling a
 * program that ctang's own parse tests contain (tests/corpus/script/
 * tests-first-program.tang), where the virtual machine runs it. The bytecode
 * of lang-tang is not involved: only ctang's executor is chosen here. Default
 * libraries and no host extras. Prints
 * either `refused` (the program does not compile) or two lines:
 *
 *     output <hex of the rendered output bytes>
 *     result <kind> <hex of the canonical text>
 *
 * The rendered output is what `gta_unicode_string_render` makes of the output
 * buffer: every segment encoded per its tag. The result is its kind and a
 * canonical text, the same rule tests/oracle/oracle.h applies to lang-tang's
 * result, so that the two lines are comparable byte for byte:
 *
 *  - null, function, library, rng: the empty text;
 *  - bool: `true` or `false`; integer: decimal; float: `%.17g` of the double;
 *  - string: its bytes; error: its message (a marker is its marker text, an
 *    error is its kind and message, never the file and line it came from);
 *  - array: `<n>[` then each element as `<kind>:<hex of its canonical text>`
 *    separated by `;`, a container element being its kind and count only;
 *  - map: the count only (a map's order is D-015, and a key list would make
 *    every map result a ledger row);
 *  - a result pointer that cannot be a heap object: the kind `garbage` (a
 *    program ending in a `use` statement leaves one; D-023).
 *
 * It is run as a child process by tests/oracle/test_oracle.cpp, with a
 * wall-clock kill, so that a ctang crash or hang is a verdict and not the end
 * of the test that asked. Exit status: 0 a verdict was printed; 2 usage; 3 the
 * file could not be read. Anything else (a signal, an abort) is ctang's.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/tang/ast/astNodeParseError.h>
#include <ghoti.io/tang/computedValue/computedValueAll.h>
#include <ghoti.io/tang/program/executionContext.h>
#include <ghoti.io/tang/program/program.h>
#include <ghoti.io/tang/tang.h>
#include <ghoti.io/tang/tangLanguage.h>
#include <ghoti.io/tang/unicodeString.h>

static char * read_file(const char * path) {
  FILE * file = fopen(path, "rb");
  if (!file) {
    return NULL;
  }
  size_t capacity = 4096;
  size_t length = 0;
  char * buffer = malloc(capacity);
  while (buffer) {
    size_t got = fread(buffer + length, 1, capacity - length - 1, file);
    length += got;
    if (got == 0) {
      break;
    }
    if (capacity - length < 2) {
      capacity *= 2;
      char * grown = realloc(buffer, capacity);
      if (!grown) {
        free(buffer);
        buffer = NULL;
      }
      else {
        buffer = grown;
      }
    }
  }
  if (buffer && ferror(file)) {
    free(buffer);
    buffer = NULL;
  }
  fclose(file);
  if (buffer) {
    buffer[length] = '\0';
  }
  return buffer;
}

static void put_hex(const char * bytes, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    printf("%02x", (unsigned char)bytes[i]);
  }
}

typedef struct {
  const char * kind;
  char * text;      // malloc'd canonical text
  size_t length;
} Canon;

static void canon_set(Canon * c, const char * kind, const char * text, size_t length) {
  c->kind = kind;
  c->text = malloc(length + 1);
  if (!c->text) {
    fprintf(stderr, "oracle_ctang: out of memory\n");
    exit(4);
  }
  memcpy(c->text, text, length);
  c->text[length] = '\0';
  c->length = length;
}

static void canon_format(Canon * c, const char * kind, const char * format, ...) __attribute__((format(printf, 3, 4)));
static void canon_format(Canon * c, const char * kind, const char * format, ...) {
  char buffer[512];
  va_list args;
  va_start(args, format);
  int n = vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  if (n < 0 || (size_t)n >= sizeof(buffer)) {
    n = (int)sizeof(buffer) - 1;
  }
  canon_set(c, kind, buffer, (size_t)n);
}

static void canon_value(Canon * c, GTA_Computed_Value * v, int depth);

static const char * kind_name(GTA_Computed_Value * v) {
  if (GTA_COMPUTED_VALUE_IS_NULL(v)) return "null";
  if (GTA_COMPUTED_VALUE_IS_BOOLEAN(v)) return "bool";
  if (GTA_COMPUTED_VALUE_IS_INTEGER(v)) return "integer";
  if (GTA_COMPUTED_VALUE_IS_FLOAT(v)) return "float";
  if (GTA_COMPUTED_VALUE_IS_STRING(v)) return "string";
  if (GTA_COMPUTED_VALUE_IS_ARRAY(v)) return "array";
  if (GTA_COMPUTED_VALUE_IS_MAP(v)) return "map";
  if (GTA_COMPUTED_VALUE_IS_FUNCTION(v) || GTA_COMPUTED_VALUE_IS_FUNCTION_NATIVE(v)) return "function";
  if (GTA_COMPUTED_VALUE_IS_ERROR(v)) return "error";
  if (GTA_COMPUTED_VALUE_IS_LIBRARY(v)) return "library";
  if (GTA_COMPUTED_VALUE_IS_RNG(v)) return "rng";
  return "other";
}

static void canon_value(Canon * c, GTA_Computed_Value * v, int depth) {
  const char * kind = kind_name(v);
  if (!strcmp(kind, "bool")) {
    canon_set(c, kind, ((GTA_Computed_Value_Boolean *)v)->value ? "true" : "false", ((GTA_Computed_Value_Boolean *)v)->value ? 4 : 5);
  }
  else if (!strcmp(kind, "integer")) {
    canon_format(c, kind, "%lld", (long long)((GTA_Computed_Value_Integer *)v)->value);
  }
  else if (!strcmp(kind, "float")) {
    canon_format(c, kind, "%.17g", (double)((GTA_Computed_Value_Float *)v)->value);
  }
  else if (!strcmp(kind, "string")) {
    GTA_Unicode_String * s = ((GTA_Computed_Value_String *)v)->value;
    canon_set(c, kind, s && s->buffer ? s->buffer : "", s && s->buffer ? s->byte_length : 0);
  }
  else if (!strcmp(kind, "error")) {
    const char * message = ((GTA_Computed_Value_Error *)v)->message;
    canon_set(c, kind, message ? message : "", message ? strlen(message) : 0);
  }
  else if (!strcmp(kind, "array")) {
    GTA_Computed_Value_Array * a = (GTA_Computed_Value_Array *)v;
    size_t n = a->elements->count;
    if (depth > 0) {
      canon_format(c, kind, "%zu", n);
      return;
    }
    size_t capacity = 64;
    size_t length = 0;
    char * out = malloc(capacity);
    length = (size_t)snprintf(out, capacity, "%zu[", n);
    for (size_t i = 0; i < n; ++i) {
      Canon e;
      canon_value(&e, (GTA_Computed_Value *)GTA_TYPEX_P(a->elements->data[i]), depth + 1);
      size_t need = length + strlen(e.kind) + 2 * e.length + 8;
      if (need > capacity) {
        capacity = need * 2;
        out = realloc(out, capacity);
      }
      length += (size_t)snprintf(out + length, capacity - length, "%s%s:", i ? ";" : "", e.kind);
      for (size_t k = 0; k < e.length; ++k) {
        length += (size_t)snprintf(out + length, capacity - length, "%02x", (unsigned char)e.text[k]);
      }
      free(e.text);
    }
    out[length++] = ']';
    canon_set(c, kind, out, length);
    free(out);
  }
  else if (!strcmp(kind, "map")) {
    canon_format(c, kind, "%zu", (size_t)((GTA_Computed_Value_Map *)v)->key_hash->entries);
  }
  else {
    canon_set(c, kind, "", 0);
  }
}

int main(int argc, char * argv[]) {
  bool run = false;
  bool script = false;
  if (argc == 3) {
    if (!strcmp(argv[1], "script") || !strcmp(argv[1], "template")) {
      script = !strcmp(argv[1], "script");
    }
    else if (!strcmp(argv[1], "run-script") || !strcmp(argv[1], "run-template")) {
      run = true;
      script = !strcmp(argv[1], "run-script");
    }
    else {
      argc = 0;
    }
  }
  if (argc != 3) {
    fprintf(stderr, "usage: oracle_ctang <script|template|run-script|run-template> <file>\n");
    return 2;
  }
  char * source = read_file(argv[2]);
  if (!source) {
    fprintf(stderr, "oracle_ctang: cannot read %s\n", argv[2]);
    return 3;
  }

  if (!run) {
    GTA_Ast_Node * ast = script
      ? gta_tang_parse_script(source)
      : gta_tang_parse_template(source);
    if (ast && GTA_AST_IS_PARSE_ERROR(ast)) {
      printf("error\n");
    }
    else {
      printf("ok %zu\n", gta_tang_node_count(ast));
    }
    fflush(stdout);
    free(source);
    return 0;
  }

  // The bytecode virtual machine only, and not the environment's choice: see
  // the comment at the top of the file.
  GTA_Language * language = gta_language_create();
  if (!language) {
    fprintf(stderr, "oracle_ctang: cannot create the language\n");
    return 4;
  }
  GTA_Program_Flags flags = GTA_PROGRAM_FLAG_DISABLE_BINARY | GTA_PROGRAM_FLAG_IGNORE_ENVIRONMENT | (script ? 0 : GTA_PROGRAM_FLAG_IS_TEMPLATE);
  GTA_Program * program = gta_program_create_with_flags(language, source, flags);
  if (!program) {
    printf("refused\n");
    fflush(stdout);
    return 0;
  }
  GTA_Execution_Context * context = gta_execution_context_create(program);
  if (!context) {
    fprintf(stderr, "oracle_ctang: cannot create the execution context\n");
    return 4;
  }
  (void)gta_program_execute(context);

  printf("output ");
  if (context->output) {
    GTA_Unicode_Rendered_String rendered = gta_unicode_string_render(context->output);
    if (rendered.buffer) {
      put_hex(rendered.buffer, rendered.length);
    }
  }
  printf("\nresult ");
  Canon canon;
  // A program that ends in a `use` statement leaves ctang's result pointing
  // at whatever lay on top of its stack (a small odd number in every case
  // seen), and reading through it kills the process. A pointer that cannot be
  // a heap object is reported as what it is rather than followed.
  uintptr_t address = (uintptr_t)context->result;
  if (address != 0 && (address < 0x10000 || (address & 7u))) {
    canon_set(&canon, "garbage", "", 0);
  }
  else if (context->result) {
    canon_value(&canon, context->result, 0);
  }
  else {
    canon_set(&canon, "null", "", 0);
  }
  printf("%s ", canon.kind);
  put_hex(canon.text, canon.length);
  printf("\n");
  fflush(stdout);
  return 0;
}
