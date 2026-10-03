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
 * The `tang` command: parse a template or a script and print its syntax tree.
 *
 * Execution arrives with the interpreter. Until then this is the way to see
 * what the front end made of a source, and the host the CLI test drives.
 *
 * Exit status: 0 the source parsed (an empty source is an empty tree); 1 the
 * source was refused (a syntax error, printed as `name:line:column: message`
 * on stderr, or a parser limit); 2 a usage error; 3 the source could not be
 * read; 4 out of memory.
 */

#include <ghoti.io/lang-tang/macros.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <ghoti.io/cutil/array.h>
#include <ghoti.io/cutil/file.h>
#include <ghoti.io/cutil/memory.h>
#include <ghoti.io/lang-tang/lang-tang.h>

#define EXIT_REFUSED 1
#define EXIT_USAGE 2
#define EXIT_READ 3
#define EXIT_MEMORY 4

static void print_help_text(void) {
  printf(
    "Usage: tang [OPTIONS] [FILE]\n"
    "\n"
    "Parse a Tang template (or script) and print its syntax tree.  With no FILE\n"
    "and no --evaluate, the source is read from stdin.  Execution arrives with\n"
    "the interpreter; until then this command parses and dumps only.\n"
    "\n"
    "  --evaluate SOURCE, -e SOURCE  Parse SOURCE instead of a file or stdin\n"
    "  --script, -s                  Parse as a script rather than a template\n"
    "  --cleanup, -c                 Accepted for ctang compatibility; this\n"
    "                                command always releases what it allocates\n"
    "  --help, -h                    Display this help message\n"
    "\n"
    "Exit status: 0 parsed; 1 refused (name:line:column: message on stderr);\n"
    "2 usage error; 3 the source could not be read; 4 out of memory.\n");
}


// Reads stdin to its end into a NUL-terminated buffer, returned in *out on
// success. The array owns the length and the growth.
static int read_stdin(char ** out) {
  GCU_Array * input = gcu_array_create(1, 1024, gltang_allocator());
  if (!input) {
    return EXIT_MEMORY;
  }
  char chunk[4096];
  size_t read_count;
  while ((read_count = fread(chunk, 1, sizeof(chunk), stdin)) > 0) {
    if (!gcu_array_append_n(input, chunk, read_count)) {
      gcu_array_destroy(input);
      return EXIT_MEMORY;
    }
  }
  if (ferror(stdin)) {
    gcu_array_destroy(input);
    return EXIT_READ;
  }
  // The terminator is in the buffer but not in the source, which is what lets
  // an empty stdin parse as an empty tree rather than as whatever follows it
  // in memory.
  if (!gcu_array_append(input, "")) {
    gcu_array_destroy(input);
    return EXIT_MEMORY;
  }
  *out = gcu_array_steal(input, NULL);
  gcu_array_destroy(input);
  return 0;
}


int main(int argc, const char * argv[]) {
  const char * file_name = NULL;
  const char * eval = NULL;
  bool is_script = false;

  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--evaluate") || !strcmp(argv[i], "-e")) {
      if (i + 1 >= argc) {
        fprintf(stderr, "tang: %s needs an argument\n", argv[i]);
        return EXIT_USAGE;
      }
      eval = argv[++i];
    }
    else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
      print_help_text();
      return 0;
    }
    else if (!strcmp(argv[i], "--script") || !strcmp(argv[i], "-s")) {
      is_script = true;
    }
    else if (!strcmp(argv[i], "--cleanup") || !strcmp(argv[i], "-c")) {
      // Accepted and ignored: see print_help_text().
    }
    else if (argv[i][0] == '-' && argv[i][1] != '\0') {
      fprintf(stderr, "tang: unknown option %s\n", argv[i]);
      return EXIT_USAGE;
    }
    else if (!file_name) {
      file_name = argv[i];
    }
    else {
      fprintf(stderr, "tang: too many arguments\n");
      return EXIT_USAGE;
    }
  }
  if (eval && file_name) {
    fprintf(stderr, "tang: give either a file name or --evaluate, not both\n");
    return EXIT_USAGE;
  }

  char * buffer = NULL;
  const char * name = "<stdin>";
  const char * source = eval;
  if (eval) {
    name = "<evaluate>";
  }
  else if (file_name) {
    // cutil reads the whole file, in chunks rather than by seeking to the end
    // first, so this works on a pipe as well as on an ordinary file, and puts
    // a NUL one past the end.
    void * contents = NULL;
    size_t length = 0;
    GCU_File_Result result = gcu_file_read(file_name, GCU_FILE_UNLIMITED, gltang_allocator(), &contents, &length);
    if (result != GCU_FILE_OK) {
      fprintf(stderr, "tang: failed to read the file %s: %s\n", file_name, gcu_file_result_string(result));
      return EXIT_READ;
    }
    buffer = contents;
    source = buffer;
    name = file_name;
    // The parser takes a NUL-terminated string, so a NUL inside the file would
    // end the source early and the rest would be dropped without a word.
    if (strlen(buffer) != length) {
      fprintf(stderr, "tang: %s contains a NUL byte\n", file_name);
      gcu_free(buffer);
      return EXIT_READ;
    }
  }
  else {
    int failure = read_stdin(&buffer);
    if (failure) {
      fprintf(stderr, failure == EXIT_READ ? "tang: failed to read from stdin\n" : "tang: out of memory\n");
      return failure;
    }
    source = buffer;
  }

  GLTANG_Tree * tree = NULL;
  GLTANG_ParseError error = {0, 0, {0}};
  GLTANG_Result result = gltang_parse(source, is_script ? GLTANG_PARSE_SCRIPT : GLTANG_PARSE_TEMPLATE, &error, &tree);
  int status = 0;
  if (result == GLTANG_OK) {
    gltang_tree_print(tree);
    gltang_tree_destroy(tree);
  }
  else if (result == GLTANG_ERR_FORMAT) {
    fprintf(stderr, "%s:%d:%d: %s\n", name, error.line, error.column, error.message);
    status = EXIT_REFUSED;
  }
  else {
    fprintf(stderr, "%s: %s\n", name, gltang_result_string(result));
    status = (result == GLTANG_ERR_OOM) ? EXIT_MEMORY : EXIT_REFUSED;
  }
  if (buffer) {
    gcu_free(buffer);
  }
  return status;
}
