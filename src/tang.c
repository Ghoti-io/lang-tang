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
 * The `tang` command: run a template or a script and write what it printed.
 *
 * A host (AD-2): it makes the group, the options, the context and the heap
 * with runtime-core and runtime-heap, compiles the source, and runs it with
 * `grcore_run`. The output is written rendered, every segment encoded per its
 * tag, so `print("<b>" + !"<i>")` writes `<b>&lt;i&gt;`. `--tree` keeps the old
 * behaviour of parsing and dumping the syntax tree.
 *
 * Exit status: 0 the program ran to its end (an empty source is an empty
 * program); 1 the source was refused (a syntax or compile error, printed as
 * `name:line:column: message` on stderr, or a parser limit); 2 a usage error;
 * 3 the source could not be read; 4 out of memory; 5 the run paused (the
 * command has no one to resume it) and 6 it was unwound by a limit.
 */

#include <ghoti.io/lang-tang/macros.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <ghoti.io/cutil/array.h>
#include <ghoti.io/cutil/file.h>
#include <ghoti.io/cutil/memory.h>
#include <ghoti.io/lang-tang/lang-tang.h>
#include <ghoti.io/lang-tang/compile.h>
#include <ghoti.io/lang-tang/execution.h>
#include <ghoti.io/runtime-core/runtime-core.h>
#include <ghoti.io/runtime-heap/runtime-heap.h>
#include <errno.h>
#include <stdlib.h>

#define EXIT_REFUSED 1
#define EXIT_USAGE 2
#define EXIT_READ 3
#define EXIT_MEMORY 4
#define EXIT_PAUSED 5
#define EXIT_UNWOUND 6
#define EXIT_SETUP 7

/** ctang's default for the deepest a call may nest (language reference 10.3). */
#define DEFAULT_CALL_DEPTH 512

static void print_help_text(void) {
  printf(
    "Usage: tang [OPTIONS] [FILE]\n"
    "\n"
    "Run a Tang template (or script) and write its output.  With no FILE and no\n"
    "--evaluate, the source is read from stdin.  The output is written rendered:\n"
    "every piece of text is encoded as its tag says.\n"
    "\n"
    "  --evaluate SOURCE, -e SOURCE  Run SOURCE, as a script, instead of a file or stdin\n"
    "  --script, -s                  Run a file or stdin as a script rather than a template\n"
    "  --template, -t                Run the --evaluate source as a template\n"
    "  --tree                        Parse and print the syntax tree; run nothing\n"
    "  --fuel N                      Give the run N units of fuel; if it is not\n"
    "                                finished by then it pauses (exit status 5); a\n"
    "                                limit reached inside one operation (a huge\n"
    "                                repeat or copy) unwinds it instead (exit 6)\n"
    "  --depth N                     Allow calls to nest N deep (default %d)\n"
    "  --cleanup, -c                 Accepted for ctang compatibility; this\n"
    "                                command always releases what it allocates\n"
    "  --help, -h                    Display this help message\n"
    "\n"
    "Exit status: 0 ran; 1 refused (name:line:column: message on stderr);\n"
    "2 usage error; 3 the source could not be read; 4 out of memory;\n"
    "5 paused at a poll (reported on stderr); 6 unwound by a limit;\n"
    "7 the runtime could not be set up for a reason other than memory.\n",
    DEFAULT_CALL_DEPTH);
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


static bool parse_count(const char * text, uint64_t * out) {
  if (!text[0]) {
    return false;
  }
  char * end = NULL;
  errno = 0;
  unsigned long long value = strtoull(text, &end, 10);
  if (errno || *end || text[0] == '-') {
    return false;
  }
  *out = (uint64_t)value;
  return true;
}

// Compiles and runs a parsed source, and writes the rendered output. The
// command is a host: it does what any host does (a group, options, a context,
// a heap, an execution, `grcore_run`) and nothing more.
static int run_tree(const GLTANG_Tree * tree, const char * name, bool has_fuel, uint64_t fuel, uint64_t depth) {
  GLTANG_ParseError error = {0, 0, {0}};
  GLTANG_Program * program = NULL;
  GLTANG_Result compiled = gltang_compile(tree, name, &error, &program);
  if (compiled == GLTANG_ERR_FORMAT) {
    fprintf(stderr, "%s:%d:%d: %s\n", name, error.line, error.column, error.message);
    return EXIT_REFUSED;
  }
  if (compiled != GLTANG_OK) {
    fprintf(stderr, "%s: %s\n", name, gltang_result_string(compiled));
    return compiled == GLTANG_ERR_OOM ? EXIT_MEMORY : EXIT_REFUSED;
  }

  int status = EXIT_SETUP;
  const char * setup_step = "the runtime could not be set up";
  GRCORE_Group * group = NULL;
  GRCORE_Options * options = NULL;
  GRCORE_Context * context = NULL;
  GRHEAP_Options * heap_options = NULL;
  GRHEAP_Heap * heap = NULL;
  GLTANG_Execution * execution = NULL;
  GRCORE_Result step = grcore_group_create(NULL, NULL, &group);
  if (step == GRCORE_OK) {
    step = grcore_options_create(NULL, &options);
  }
  GRHEAP_Result heap_step = GRHEAP_OK;
  if (step == GRCORE_OK) {
    heap_step = grheap_options_create(NULL, &heap_options);
  }
  if (step == GRCORE_OK && heap_step == GRHEAP_OK) {
    if (has_fuel) {
      step = grcore_options_set_fuel(options, fuel);
    }
    // ctang's depth counts the calls, and the program's own frame is one more.
    if (step == GRCORE_OK) {
      step = grcore_options_set_guest_depth(options, depth == UINT64_MAX ? depth : depth + 1u);
    }
    if (step == GRCORE_OK && gltang_heap_options_configure(heap_options) != GLTANG_OK) {
      step = GRCORE_ERR_INTERNAL;
    }
  }
  if (step == GRCORE_OK && heap_step == GRHEAP_OK) {
    step = grcore_context_create(group, options, &context);
    if (step == GRCORE_OK) {
      heap_step = grheap_heap_create(context, heap_options, &heap);
    }
  }
  GLTANG_Result created = GLTANG_OK;
  if (step == GRCORE_OK && heap_step == GRHEAP_OK) {
    created = gltang_execution_create(context, program, &execution);
  }
  if (step != GRCORE_OK || heap_step != GRHEAP_OK || created != GLTANG_OK) {
    if (step == GRCORE_ERR_OOM || heap_step == GRHEAP_ERR_OOM || created == GLTANG_ERR_OOM) {
      status = EXIT_MEMORY;
    }
    else {
      fprintf(stderr, "%s: %s: %s\n", name, setup_step,
          step != GRCORE_OK ? grcore_result_string(step)
          : heap_step != GRHEAP_OK ? grheap_result_string(heap_step)
          : gltang_result_string(created));
    }
    goto done;
  }

  GRCORE_Outcome outcome = GRCORE_OUTCOME_FINISHED;
  GRCORE_Result ran = grcore_run(context, gltang_execution_entry, execution, &outcome);
  char * text = NULL;
  size_t length = 0;
  if (gltang_execution_output_render(execution, &text, &length) == GLTANG_OK) {
    fwrite(text, 1, length, stdout);
    gltang_buffer_free(text);
  }
  else {
    // The output could not be built, so what the program printed is lost:
    // that is the same failure as running out of memory, and not a success.
    fflush(stdout);
    status = EXIT_MEMORY;
    goto done;
  }
  fflush(stdout);
  if (ran == GRCORE_OK && outcome == GRCORE_OUTCOME_FINISHED) {
    status = 0;
  }
  else if (ran == GRCORE_OK) {
    GRCORE_Location where = grcore_context_pause_location(context);
    fprintf(stderr, "%s:%d: paused", where.file ? where.file : name, where.line);
    for (size_t k = 0; k < grcore_context_pause_key_count(context); ++k) {
      const GRCORE_Key * key = grcore_context_pause_key(context, k);
      fprintf(stderr, "%s%s", k ? ", " : " on ", key && key->name ? key->name : "?");
    }
    fputc('\n', stderr);
    status = EXIT_PAUSED;
  }
  else {
    fprintf(stderr, "%s: the run was stopped: %s\n", name, grcore_result_string(ran));
    status = EXIT_UNWOUND;
  }

done:
  if (status == EXIT_MEMORY) {
    fprintf(stderr, "%s: out of memory\n", name);
  }
  if (context) {
    grcore_context_destroy(context);
  }
  grheap_options_destroy(heap_options);
  grcore_options_destroy(options);
  if (group) {
    grcore_group_destroy(group);
  }
  gltang_program_release(program);
  return status;
}


int main(int argc, const char * argv[]) {
  const char * file_name = NULL;
  const char * eval = NULL;
  bool is_script = false;
  bool is_template = false;
  bool dump_tree = false;
  bool has_fuel = false;
  uint64_t fuel = 0;
  uint64_t depth = DEFAULT_CALL_DEPTH;

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
    else if (!strcmp(argv[i], "--template") || !strcmp(argv[i], "-t")) {
      is_template = true;
    }
    else if (!strcmp(argv[i], "--cleanup") || !strcmp(argv[i], "-c")) {
      // Accepted and ignored: see print_help_text().
    }
    else if (!strcmp(argv[i], "--tree")) {
      dump_tree = true;
    }
    else if (!strcmp(argv[i], "--fuel") || !strcmp(argv[i], "--depth")) {
      bool is_fuel = !strcmp(argv[i], "--fuel");
      if (i + 1 >= argc || !parse_count(argv[i + 1], is_fuel ? &fuel : &depth)) {
        fprintf(stderr, "tang: %s needs a number\n", argv[i]);
        return EXIT_USAGE;
      }
      has_fuel = has_fuel || is_fuel;
      ++i;
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
  if (is_script && is_template) {
    fprintf(stderr, "tang: give either --script or --template, not both\n");
    return EXIT_USAGE;
  }
  // The source given with --evaluate is code (`tang -e 'print(1+2);'` prints
  // 3); a file or stdin is a template unless it is told otherwise.
  if (eval && !is_template) {
    is_script = true;
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
    if (dump_tree) {
      gltang_tree_print(tree);
    }
    else {
      status = run_tree(tree, name, has_fuel, fuel, depth);
    }
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
