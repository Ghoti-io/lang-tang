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
 * Natives that touch the program's values: read an array, answer a container,
 * bind a function to an object, answer a library.
 *
 * A native sees the program's values only through *handles* (handle.h), indexes
 * into a scope the engine roots for the length of the call, so it survives the
 * collector moving things and never holds a pointer into the heap:
 *
 *  - `titles(rows)` reads an array of maps and answers an array of the titles it
 *    found, each string keeping the encoding it came in with;
 *  - `account(name)` answers a map whose `label` is a native function bound to the
 *    map, so `a.label()` knows which account it is on;
 *  - `pages` is a library whose `recent` member is made by a factory, when a
 *    program first reaches it, and is itself a library.
 */

#include <ghoti.io/lang-tang/compile.h>
#include <ghoti.io/lang-tang/execution.h>
#include <ghoti.io/lang-tang/handle.h>
#include <ghoti.io/lang-tang/lang-tang.h>
#include <ghoti.io/lang-tang/library.h>
#include <ghoti.io/lang-tang/program.h>
#include <ghoti.io/lang-tang/value.h>
#include <ghoti.io/runtime-core/runtime-core.h>
#include <ghoti.io/runtime-heap/runtime-heap.h>

#include <stdio.h>
#include <string.h>

/* Reads an array of maps and answers the `title` of each, in an array. */
static bool titles(GLTANG_NativeCall * call, void * user) {
  (void)user;
  GLTANG_Handle rows, found;
  if (gltang_call_arg(call, 0, &rows) != GLTANG_OK || gltang_handle_make_array(call, 0, &found) != GLTANG_OK) {
    return false;
  }
  for (size_t i = 0; i < gltang_handle_array_length(call, rows); ++i) {
    /* The handles of one row end with its frame, so a long array needs few. */
    GLTANG_Handle row, title;
    bool present;
    if (gltang_handle_push_frame(call) != GLTANG_OK || gltang_handle_array_get(call, rows, i, &row) != GLTANG_OK
        || gltang_handle_map_get(call, row, "title", 5, &present, &title) != GLTANG_OK) {
      return false;
    }
    if (present && gltang_handle_kind(call, title) == GLTANG_KIND_STRING && gltang_handle_array_push(call, found, title) != GLTANG_OK) {
      return false;   /* the call's value is Out of memory */
    }
    (void)gltang_handle_pop_frame(call);
  }
  gltang_call_return_handle(call, found);
  return true;
}

/* The method: `a.label()`. It is bound to the map it came from. */
static bool label(GLTANG_NativeCall * call, void * user) {
  (void)user;
  GLTANG_Handle self, name, joined, prefix;
  bool present;
  if (gltang_call_bound(call, &self) != GLTANG_OK || gltang_handle_map_get(call, self, "name", 4, &present, &name) != GLTANG_OK || !present
      || gltang_handle_make_string(call, "account: ", 9, GLTANG_UNICODE_STRING_TYPE_TRUSTED, &prefix) != GLTANG_OK
      || gltang_handle_make_concat(call, prefix, name, &joined) != GLTANG_OK) {
    return false;
  }
  gltang_call_return_handle(call, joined);   /* the name keeps its own tag */
  return true;
}

static bool account(GLTANG_NativeCall * call, void * user) {
  (void)user;
  GLTANG_Handle name, object, method;
  if (gltang_call_arg(call, 0, &name) != GLTANG_OK || gltang_handle_make_map(call, 2, &object) != GLTANG_OK
      || gltang_handle_map_set(call, object, "name", 4, GLTANG_UNICODE_STRING_TYPE_TRUSTED, name) != GLTANG_OK
      || gltang_handle_make_native(call, label, NULL, object, &method) != GLTANG_OK
      || gltang_handle_map_set(call, object, "label", 5, GLTANG_UNICODE_STRING_TYPE_TRUSTED, method) != GLTANG_OK) {
    return false;
  }
  gltang_call_return_handle(call, object);
  return true;
}

/* A factory that answers a library, when a program reaches `pages.recent`. */
static int recent_made;

static bool recent_factory(GLTANG_NativeCall * call, void * user) {
  (void)user;
  ++recent_made;
  GLTANG_Library * recent = NULL;
  GLTANG_Handle h;
  bool ok = gltang_library_create("recent", &recent) == GLTANG_OK
      && gltang_library_add_string(recent, "first", "Hello <world>", 13, GLTANG_UNICODE_STRING_TYPE_HTML) == GLTANG_OK
      && gltang_handle_make_library(call, recent, &h) == GLTANG_OK;
  gltang_library_release(recent);   /* the execution keeps its own reference */
  if (ok) {
    gltang_call_return_handle(call, h);
  }
  return ok;
}

static char * render(GLTANG_Program * program) {
  GRCORE_Group * group = NULL;
  GRCORE_Options * options = NULL;
  GRHEAP_Options * heap_options = NULL;
  GRCORE_Context * context = NULL;
  GRHEAP_Heap * heap = NULL;
  GLTANG_Execution * execution = NULL;
  GLTANG_Library * root = NULL;
  GLTANG_Library * pages = NULL;
  char * rendered = NULL;

  if (grcore_group_create(NULL, NULL, &group) != GRCORE_OK || grcore_options_create(NULL, &options) != GRCORE_OK || grheap_options_create(NULL, &heap_options) != GRHEAP_OK) {
    goto done;
  }
  if (gltang_heap_options_configure(heap_options) != GLTANG_OK || grcore_context_create(group, options, &context) != GRCORE_OK
      || grheap_heap_create(context, heap_options, &heap) != GRHEAP_OK || gltang_execution_create(context, program, &execution) != GLTANG_OK) {
    goto done;
  }
  if (gltang_library_create(NULL, &root) != GLTANG_OK || gltang_library_create("pages", &pages) != GLTANG_OK
      || gltang_library_add_handle_factory(pages, "recent", recent_factory, NULL) != GLTANG_OK || gltang_library_add_library(root, pages) != GLTANG_OK
      || gltang_library_add_native(root, "titles", titles, NULL) != GLTANG_OK || gltang_library_add_native(root, "account", account, NULL) != GLTANG_OK
      || gltang_execution_set_libraries(execution, root) != GLTANG_OK) {
    goto done;
  }
  GRCORE_Outcome outcome;
  if (grcore_run(context, gltang_execution_entry, execution, &outcome) != GRCORE_OK || outcome != GRCORE_OUTCOME_FINISHED) {
    goto done;
  }
  if (gltang_execution_output_render(execution, &rendered, NULL) != GLTANG_OK) {
    rendered = NULL;
  }

done:
  gltang_library_release(pages);
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
    "<% use titles; use account; use pages.recent as recent; "
    "rows = [{title: recent.first}, {nothing: 1}, {title: \"Tom & Jerry\"}]; "
    "a = account(\"Ann <ann@example.com>\"); b = account(\"Bob\"); %>"
    "<ul><% for (t : titles(rows)) { %><li><%= t %></li><% } %></ul>\n"
    "<p><%= a[\"label\"]() %> / <%= b[\"label\"]() %></p>\n";
  int status = 1;

  GLTANG_Tree * tree = NULL;
  GLTANG_Program * program = NULL;
  char * page = NULL;

  GLTANG_ParseError error;
  if (gltang_parse(source, GLTANG_PARSE_TEMPLATE, &error, &tree) != GLTANG_OK || gltang_compile(tree, "page.tang", &error, &program) != GLTANG_OK) {
    fprintf(stderr, "refused: %d:%d: %s\n", error.line, error.column, error.message);
    goto done;
  }
  page = render(program);
  if (!page) {
    fprintf(stderr, "the page could not be rendered\n");
    goto done;
  }
  printf("%s", page);
  /* The titles kept the encodings they came in with: the first is the library's
   * HTML text and is escaped, the second is the template's own literal and is
   * trusted; each bound label reads its own object. */
  if (!strstr(page, "<li>Hello &lt;world&gt;</li>") || !strstr(page, "<li>Tom & Jerry</li>") || strstr(page, "<li></li>")
      || !strstr(page, "account: Ann <ann@example.com>") || !strstr(page, "account: Bob") || recent_made != 1) {
    fprintf(stderr, "the page is not what the natives and the tags say\n");
    goto done;
  }
  status = 0;

done:
  gltang_buffer_free(page);
  gltang_program_release(program);
  gltang_tree_destroy(tree);
  return status;
}
