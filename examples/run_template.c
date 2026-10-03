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
 * Parse, compile and run a template, and read what it printed.
 *
 * The steps are the whole path from source to output: parse to a tree,
 * compile the tree to a program, build the runtime a program needs (a group,
 * a context, a heap, an execution), run, and read the result and the output.
 * The template's own text is trusted and goes out as it is; each name is
 * printed as HTML text (`.html`), so the rendered output (each piece encoded
 * for its tag) differs from the raw one.
 */

#include <ghoti.io/lang-tang/compile.h>
#include <ghoti.io/lang-tang/execution.h>
#include <ghoti.io/lang-tang/lang-tang.h>
#include <ghoti.io/lang-tang/program.h>
#include <ghoti.io/lang-tang/value.h>
#include <ghoti.io/runtime-core/runtime-core.h>
#include <ghoti.io/runtime-heap/runtime-heap.h>

#include <stdio.h>
#include <string.h>

/* Returns 0 on success. Every failure names the step that failed. */
int main(void) {
  const char * source =
    "<ul>\n"
    "<% names = [\"Ada\", \"<Bob>\", \"Cy & Di\"]; %>"
    "<% for (name : names) { %><li><%= name.html %></li>\n<% } %>"
    "</ul>\n";
  int status = 1;

  GLTANG_Tree * tree = NULL;
  GLTANG_Program * program = NULL;
  GRCORE_Group * group = NULL;
  GRCORE_Options * options = NULL;
  GRHEAP_Options * heap_options = NULL;
  GRCORE_Context * context = NULL;
  GRHEAP_Heap * heap = NULL;
  GLTANG_Execution * execution = NULL;
  char * rendered = NULL;

  /* Source to tree, tree to program. A refusal says where and why. */
  GLTANG_ParseError error;
  if (gltang_parse(source, GLTANG_PARSE_TEMPLATE, &error, &tree) != GLTANG_OK) {
    fprintf(stderr, "parse: %d:%d: %s\n", error.line, error.column, error.message);
    goto done;
  }
  if (gltang_compile(tree, "page.tang", &error, &program) != GLTANG_OK) {
    fprintf(stderr, "compile: %d:%d: %s\n", error.line, error.column, error.message);
    goto done;
  }

  /* The runtime. The heap must be told how lang-tang tells a pointer from a
   * number, which is what gltang_heap_options_configure does. A budget of
   * fuel bounds the run; here it is generous. */
  if (grcore_group_create(NULL, NULL, &group) != GRCORE_OK
      || grcore_options_create(NULL, &options) != GRCORE_OK
      || grheap_options_create(NULL, &heap_options) != GRHEAP_OK) {
    fprintf(stderr, "setup: out of memory\n");
    goto done;
  }
  grcore_options_set_fuel(options, 1000000);
  if (gltang_heap_options_configure(heap_options) != GLTANG_OK
      || grcore_context_create(group, options, &context) != GRCORE_OK
      || grheap_heap_create(context, heap_options, &heap) != GRHEAP_OK
      || gltang_execution_create(context, program, &execution) != GLTANG_OK) {
    fprintf(stderr, "setup: could not build the context\n");
    goto done;
  }

  GRCORE_Outcome outcome;
  if (grcore_run(context, gltang_execution_entry, execution, &outcome) != GRCORE_OK
      || outcome != GRCORE_OUTCOME_FINISHED) {
    fprintf(stderr, "run: the template did not finish\n");
    goto done;
  }

  /* The raw output is every piece unencoded; the rendered output encodes each
   * piece for the tag it was printed under. */
  size_t raw_length = 0;
  const char * raw = gltang_execution_output_raw(execution, &raw_length);
  size_t rendered_length = 0;
  if (gltang_execution_output_render(execution, &rendered, &rendered_length) != GLTANG_OK) {
    fprintf(stderr, "render: out of memory\n");
    goto done;
  }
  printf("%s", rendered);

  if (strstr(raw, "<Bob>") == NULL || strstr(rendered, "&lt;Bob&gt;") == NULL
      || strstr(rendered, "Cy &amp; Di") == NULL) {
    fprintf(stderr, "the output is not what the template says\n");
    goto done;
  }
  status = 0;

done:
  gltang_buffer_free(rendered);
  if (context) {
    grcore_context_destroy(context);
  }
  grheap_options_destroy(heap_options);
  grcore_options_destroy(options);
  if (group) {
    grcore_group_destroy(group);
  }
  gltang_program_release(program);
  gltang_tree_destroy(tree);
  return status;
}
