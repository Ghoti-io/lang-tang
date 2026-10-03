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
 * Usage: oracle_ctang <script|template> <file>
 *
 * Parses the file with frozen ctang's own parse entry points and prints one
 * line: `ok <node-count>` when it accepts the source (an empty source is
 * `ok 0`, as ctang's NULL meant "nothing to parse"), or `error` when it
 * refuses. The interpreter path is not used: this commit compares parse
 * verdicts, which is all both sides can do yet.
 *
 * It is run as a child process by tests/oracle/test_oracle.cpp, with a
 * wall-clock kill, so that a ctang crash or hang is a verdict and not the end
 * of the test that asked. Exit status: 0 a verdict was printed; 2 usage; 3 the
 * file could not be read. Anything else (a signal, an abort) is ctang's.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/tang/ast/astNodeParseError.h>
#include <ghoti.io/tang/tangLanguage.h>

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

int main(int argc, char * argv[]) {
  if (argc != 3 || (strcmp(argv[1], "script") && strcmp(argv[1], "template"))) {
    fprintf(stderr, "usage: oracle_ctang <script|template> <file>\n");
    return 2;
  }
  char * source = read_file(argv[2]);
  if (!source) {
    fprintf(stderr, "oracle_ctang: cannot read %s\n", argv[2]);
    return 3;
  }

  GTA_Ast_Node * ast = !strcmp(argv[1], "script")
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
