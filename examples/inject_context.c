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
 * Inject a context: one compiled template, two executions, two users.
 *
 * A template is compiled once, and the program is immutable and shared. Each
 * request gets its own execution, and the host gives it the data it needs as a
 * library: here a `user` library of scalars (a name, an id, an administrator
 * flag) and a native function, `greet`, that a program calls like any other
 * function. The same program prints a page for each user.
 *
 * Two things to take from it. A library is sealed the moment it is attached, so
 * it could be shared by many executions on many threads with no lock. And a
 * native function is opaque: it is given the arguments and its own `user`
 * pointer, answers with one value, and cannot call back into the program.
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

/* The host's own state, which a native function sees through its `user`
 * pointer: here, the greeting it prefixes. */
typedef struct {
  const char * greeting;
} Site;

static bool greet(GLTANG_NativeCall * call, void * user) {
  const Site * site = user;
  size_t length = 0;
  const char * name = gltang_call_text(call, 0, &length);
  if (gltang_call_count(call) != 1 || !name) {
    gltang_call_return_error(call, GLTANG_ERROR_INVALID_FUNCTION_CALL);
    return true;
  }
  char text[128];
  int n = snprintf(text, sizeof(text), "%s, %.*s!", site->greeting, (int)length, name);
  /* The answer carries its encoding: a name from a user is HTML text, so the
   * page escapes it. */
  gltang_call_return_string(call, text, (size_t)n, GLTANG_UNICODE_STRING_TYPE_HTML);
  return true;
}

/* Runs the program for one user and returns the rendered page, which the caller
 * releases with gltang_buffer_free. NULL on any failure. */
static char * render_for(GLTANG_Program * program, const char * name, int64_t id, bool admin, Site * site) {
  GRCORE_Group * group = NULL;
  GRCORE_Options * options = NULL;
  GRHEAP_Options * heap_options = NULL;
  GRCORE_Context * context = NULL;
  GRHEAP_Heap * heap = NULL;
  GLTANG_Execution * execution = NULL;
  GLTANG_Library * root = NULL;
  GLTANG_Library * user = NULL;
  char * rendered = NULL;

  if (grcore_group_create(NULL, NULL, &group) != GRCORE_OK
      || grcore_options_create(NULL, &options) != GRCORE_OK
      || grheap_options_create(NULL, &heap_options) != GRHEAP_OK) {
    goto done;
  }
  grcore_options_set_fuel(options, 100000);
  if (gltang_heap_options_configure(heap_options) != GLTANG_OK
      || grcore_context_create(group, options, &context) != GRCORE_OK
      || grheap_heap_create(context, heap_options, &heap) != GRHEAP_OK
      || gltang_execution_create(context, program, &execution) != GLTANG_OK) {
    goto done;
  }

  /* The execution's libraries: the `user` library, and `greet` beside it. The
   * root library has no name; `user` is a member of it under its own name. */
  if (gltang_library_create(NULL, &root) != GLTANG_OK
      || gltang_library_create("user", &user) != GLTANG_OK
      || gltang_library_add_string(user, "name", name, strlen(name), GLTANG_UNICODE_STRING_TYPE_HTML) != GLTANG_OK
      || gltang_library_add_integer(user, "id", id) != GLTANG_OK
      || gltang_library_add_bool(user, "admin", admin) != GLTANG_OK
      || gltang_library_add_library(root, user) != GLTANG_OK
      || gltang_library_add_native(root, "greet", greet, site) != GLTANG_OK
      || gltang_execution_set_libraries(execution, root) != GLTANG_OK) {
    goto done;
  }

  GRCORE_Outcome outcome;
  if (grcore_run(context, gltang_execution_entry, execution, &outcome) != GRCORE_OK
      || outcome != GRCORE_OUTCOME_FINISHED) {
    goto done;
  }
  if (gltang_execution_output_render(execution, &rendered, NULL) != GLTANG_OK) {
    rendered = NULL;
  }

done:
  gltang_library_release(user);
  gltang_library_release(root);
  if (context) {
    grcore_context_destroy(context);
  }
  grheap_options_destroy(heap_options);
  grcore_options_destroy(options);
  if (group) {
    grcore_group_destroy(group);
  }
  return rendered;
}

int main(void) {
  const char * source =
    "<% use user; use greet; %>"
    "<h1><%= greet(user.name) %></h1>\n"
    "<p>Account <%= user.id %><% if (user.admin) { %> (administrator)<% } %></p>\n";
  int status = 1;

  GLTANG_Tree * tree = NULL;
  GLTANG_Program * program = NULL;
  char * first = NULL;
  char * second = NULL;
  Site site = {"Welcome"};

  GLTANG_ParseError error;
  if (gltang_parse(source, GLTANG_PARSE_TEMPLATE, &error, &tree) != GLTANG_OK
      || gltang_compile(tree, "greeting.tang", &error, &program) != GLTANG_OK) {
    fprintf(stderr, "refused: %d:%d: %s\n", error.line, error.column, error.message);
    goto done;
  }

  /* The program is compiled once and runs in two contexts. */
  first = render_for(program, "Ann <ann@example.com>", 7, true, &site);
  second = render_for(program, "Bob", 12, false, &site);
  if (!first || !second) {
    fprintf(stderr, "a page could not be rendered\n");
    goto done;
  }
  printf("--- ann\n%s--- bob\n%s", first, second);

  if (!strstr(first, "Welcome, Ann &lt;ann@example.com&gt;!") || !strstr(first, "Account 7 (administrator)")
      || !strstr(second, "Welcome, Bob!") || !strstr(second, "Account 12</p>")) {
    fprintf(stderr, "the pages are not what the template says\n");
    goto done;
  }
  status = 0;

done:
  gltang_buffer_free(first);
  gltang_buffer_free(second);
  gltang_program_release(program);
  gltang_tree_destroy(tree);
  return status;
}
