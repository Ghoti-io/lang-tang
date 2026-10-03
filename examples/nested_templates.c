/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
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
 * A page, a sidebar and a nav pane that never ends.
 *
 * A template is a value `use` binds, and calling it is a call like any other:
 * `sidebar()` runs the sidebar template on the page's own stack and the call's
 * value is what the sidebar printed. Each call opens a budget scope with its
 * own fuel. Here the nav pane loops forever, and it is stopped at its own
 * boundary after 500 units: its slot is empty, the sidebar goes on to print
 * `<aside>` and the page prints `<main>` and both finish, because a scope
 * charges only the template running in it.
 *
 * The host learns what happened from the error list: one entry, which names the
 * template that was stopped (`nav`), the chain of calls above it (the page, then
 * the sidebar, each with the line of its call) and the line the loop was on.
 */

#include <ghoti.io/lang-tang/compile.h>
#include <ghoti.io/lang-tang/execution.h>
#include <ghoti.io/lang-tang/lang-tang.h>
#include <ghoti.io/lang-tang/library.h>
#include <ghoti.io/lang-tang/program.h>
#include <ghoti.io/lang-tang/value.h>
#include <ghoti.io/runtime-core/runtime-core.h>
#include <ghoti.io/runtime-heap/runtime-heap.h>

#include <stdio.h>
#include <string.h>

static GLTANG_Program * compile(const char * source, const char * file) {
  GLTANG_Tree * tree = NULL;
  GLTANG_Program * program = NULL;
  GLTANG_ParseError error;
  if (gltang_parse(source, GLTANG_PARSE_SCRIPT, &error, &tree) != GLTANG_OK
      || gltang_compile(tree, file, &error, &program) != GLTANG_OK) {
    fprintf(stderr, "%s: %d:%d: %s\n", file, error.line, error.column, error.message);
    program = NULL;
  }
  gltang_tree_destroy(tree);
  return program;
}

int main(void) {
  int status = 1;
  GLTANG_Program * page = compile("use sidebar;\nprint(\"<main>\" + sidebar());\n", "page.tang");
  GLTANG_Program * sidebar = compile("use nav; print(nav());\nprint(\"<aside>\");\n", "sidebar.tang");
  GLTANG_Program * nav = compile("while (true) {}\n", "nav.tang");
  GRCORE_Group * group = NULL;
  GRCORE_Options * options = NULL;
  GRHEAP_Options * heap_options = NULL;
  GRCORE_Context * context = NULL;
  GRHEAP_Heap * heap = NULL;
  GLTANG_Execution * execution = NULL;
  GLTANG_Library * library = NULL;
  char * rendered = NULL;

  if (!page || !sidebar || !nav) {
    goto done;
  }
  if (grcore_group_create(NULL, NULL, &group) != GRCORE_OK
      || grcore_options_create(NULL, &options) != GRCORE_OK
      || grheap_options_create(NULL, &heap_options) != GRHEAP_OK) {
    goto done;
  }
  /* The request's whole budget: the ceiling, inclusive of every scope. */
  grcore_options_set_fuel(options, 1000000);
  if (gltang_heap_options_configure(heap_options) != GLTANG_OK
      || grcore_context_create(group, options, &context) != GRCORE_OK
      || grheap_heap_create(context, heap_options, &heap) != GRHEAP_OK
      || gltang_execution_create(context, page, &execution) != GLTANG_OK) {
    goto done;
  }

  /* The templates, each with the fuel of its own scope. The nav pane's is
   * small, and its slot is empty if it runs out (GLTANG_SCOPE_SEGMENTS would
   * leave whatever it had finished printing; GLTANG_SCOPE_PAUSE would pause
   * the run so the host could raise it). */
  if (gltang_library_create(NULL, &library) != GLTANG_OK
      || gltang_library_add_template(library, "sidebar", sidebar, 10000, GLTANG_SCOPE_EMPTY) != GLTANG_OK
      || gltang_library_add_template(library, "nav", nav, 500, GLTANG_SCOPE_EMPTY) != GLTANG_OK
      || gltang_execution_set_libraries(execution, library) != GLTANG_OK
      || gltang_execution_set_name(execution, "page") != GLTANG_OK) {
    goto done;
  }

  GRCORE_Outcome outcome;
  if (grcore_run(context, gltang_execution_entry, execution, &outcome) != GRCORE_OK
      || outcome != GRCORE_OUTCOME_FINISHED) {
    fprintf(stderr, "the page did not finish\n");
    goto done;
  }
  if (gltang_execution_output_render(execution, &rendered, NULL) != GLTANG_OK) {
    goto done;
  }
  printf("%s\n", rendered);

  /* What the host learned. */
  size_t count = gltang_execution_error_count(execution);
  for (size_t i = 0; i < count; ++i) {
    GLTANG_ErrorEntry entry;
    if (!gltang_execution_error(execution, i, &entry)) {
      continue;
    }
    printf("%s:%s:%d: %s\n", entry.template_name ? entry.template_name : "?", entry.file ? entry.file : "?", entry.line, entry.message);
    for (size_t k = 0; k < entry.chain_count; ++k) {
      GLTANG_ErrorLink link;
      if (!gltang_execution_error_chain(execution, i, k, &link)) {
        continue;
      }
      printf("  in %s:%s:%d\n", link.template_name ? link.template_name : "?", link.file ? link.file : "?", link.line);
    }
  }

  GLTANG_ErrorEntry entry;
  if (strcmp(rendered, "<main><aside>") != 0 || count != 1
      || !gltang_execution_error(execution, 0, &entry) || strcmp(entry.template_name, "nav") != 0
      || entry.how != GLTANG_ERROR_HOW_SCOPE_LIMIT || entry.chain_count != 2) {
    fprintf(stderr, "the page and the error list are not what they should be\n");
    goto done;
  }
  status = 0;

done:
  gltang_buffer_free(rendered);
  gltang_library_release(library);
  if (context) {
    grcore_context_destroy(context);
  }
  grheap_options_destroy(heap_options);
  grcore_options_destroy(options);
  if (group) {
    grcore_group_destroy(group);
  }
  gltang_program_release(nav);
  gltang_program_release(sidebar);
  gltang_program_release(page);
  return status;
}
