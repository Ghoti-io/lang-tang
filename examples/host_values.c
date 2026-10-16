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
 * Hand a template an array of maps: a menu, a count, and the answer of a search.
 *
 * A host cannot build an array or a map in the engine's heap (it belongs to one
 * execution and moves its objects), so it builds a *description* of one in its
 * own memory (description.h) and hands that over in one of three places:
 *
 *  - a library member (`menu`): built once, sealed, shared by every execution;
 *    each `use menu;` binds a fresh copy;
 *  - a lazy factory (`stats`): the engine calls it only when a `use` reaches it,
 *    with an empty description to fill;
 *  - a native function's answer (`search`): a description built during the call,
 *    from the context's own allocator, so what it costs is charged to the run.
 *
 * Every string carries the encoding it is printed in, and keeps it inside the
 * containers: the labels are HTML, the links are percent-encoded, and the page
 * escapes each as it says.
 */

#include <ghoti.io/lang-tang/compile.h>
#include <ghoti.io/lang-tang/description.h>
#include <ghoti.io/lang-tang/execution.h>
#include <ghoti.io/lang-tang/lang-tang.h>
#include <ghoti.io/lang-tang/library.h>
#include <ghoti.io/lang-tang/program.h>
#include <ghoti.io/lang-tang/value.h>
#include <ghoti.io/runtime-core/runtime-core.h>
#include <ghoti.io/runtime-heap/runtime-heap.h>

#include <stdio.h>
#include <string.h>

/* The menu, built once by the host and shared. */
static GLTANG_ValueDescription * build_menu(void) {
  static const struct {
    const char * label;
    const char * url;
  } items[] = {{"Home", "/"}, {"News & events", "/news?type=all"}, {"<Admin> area", "/admin panel"}};
  GLTANG_ValueDescription * menu = NULL;
  GLTANG_ValueRef array;
  if (gltang_value_description_create(NULL, &menu) != GLTANG_OK || gltang_value_array(menu, &array) != GLTANG_OK) {
    gltang_value_description_release(menu);
    return NULL;
  }
  for (size_t i = 0; i < sizeof items / sizeof items[0]; ++i) {
    GLTANG_ValueRef item, label, url;
    if (gltang_value_map(menu, &item) != GLTANG_OK
        || gltang_value_string(menu, items[i].label, strlen(items[i].label), GLTANG_UNICODE_STRING_TYPE_HTML, &label) != GLTANG_OK
        || gltang_value_string(menu, items[i].url, strlen(items[i].url), GLTANG_UNICODE_STRING_TYPE_PERCENT, &url) != GLTANG_OK
        || gltang_value_map_set(menu, item, "label", 5, GLTANG_UNICODE_STRING_TYPE_TRUSTED, label) != GLTANG_OK
        || gltang_value_map_set(menu, item, "url", 3, GLTANG_UNICODE_STRING_TYPE_TRUSTED, url) != GLTANG_OK
        || gltang_value_array_push(menu, array, item) != GLTANG_OK) {
      gltang_value_description_release(menu);
      return NULL;
    }
  }
  if (gltang_value_set_root(menu, array) != GLTANG_OK) {
    gltang_value_description_release(menu);
    return NULL;
  }
  return menu;  /* handing it to a library seals it */
}

/* A lazy factory: counts how often a program reached it. */
static int stats_reached;

static bool stats_factory(void * user, GLTANG_ValueDescription * out) {
  (void)user;
  ++stats_reached;
  GLTANG_ValueRef map, visits;
  return gltang_value_map(out, &map) == GLTANG_OK && gltang_value_integer(out, 40 + stats_reached, &visits) == GLTANG_OK
      && gltang_value_map_set(out, map, "visits", 6, GLTANG_UNICODE_STRING_TYPE_TRUSTED, visits) == GLTANG_OK && gltang_value_set_root(out, map) == GLTANG_OK;
}

/* A native whose answer is an array of the words it was given, each an HTML string. */
static bool search(GLTANG_NativeCall * call, void * user) {
  (void)user;
  GLTANG_ValueDescription * found = NULL;
  GLTANG_ValueRef array;
  if (gltang_call_description(call, &found) != GLTANG_OK || gltang_value_array(found, &array) != GLTANG_OK) {
    gltang_call_return_error(call, GLTANG_ERROR_HOST_FAILED);
    return true;
  }
  for (size_t i = 0; i < gltang_call_count(call); ++i) {
    size_t length = 0;
    const char * word = gltang_call_text(call, i, &length);
    GLTANG_ValueRef s;
    if (!word || gltang_value_string(found, word, length, GLTANG_UNICODE_STRING_TYPE_HTML, &s) != GLTANG_OK || gltang_value_array_push(found, array, s) != GLTANG_OK) {
      gltang_call_return_error(call, GLTANG_ERROR_INVALID_FUNCTION_CALL);
      return true;
    }
  }
  if (gltang_value_set_root(found, array) != GLTANG_OK) {
    gltang_call_return_error(call, GLTANG_ERROR_HOST_FAILED);
    return true;
  }
  gltang_call_return_value(call, found);  /* the call frees `found` when it returns */
  return true;
}

static char * render(GLTANG_Program * program, GLTANG_ValueDescription * menu) {
  GRCORE_Group * group = NULL;
  GRCORE_Options * options = NULL;
  GRHEAP_Options * heap_options = NULL;
  GRCORE_Context * context = NULL;
  GRHEAP_Heap * heap = NULL;
  GLTANG_Execution * execution = NULL;
  GLTANG_Library * root = NULL;
  char * rendered = NULL;

  if (grcore_group_create(NULL, NULL, &group) != GRCORE_OK || grcore_options_create(NULL, &options) != GRCORE_OK || grheap_options_create(NULL, &heap_options) != GRHEAP_OK) {
    goto done;
  }
  if (gltang_heap_options_configure(heap_options) != GLTANG_OK || grcore_context_create(group, options, &context) != GRCORE_OK
      || grheap_heap_create(context, heap_options, &heap) != GRHEAP_OK || gltang_execution_create(context, program, &execution) != GLTANG_OK) {
    goto done;
  }
  if (gltang_library_create(NULL, &root) != GLTANG_OK || gltang_library_add_value(root, "menu", menu) != GLTANG_OK
      || gltang_library_add_value_factory(root, "stats", stats_factory, NULL) != GLTANG_OK || gltang_library_add_native(root, "search", search, NULL) != GLTANG_OK
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
    "<% use menu; use stats; use search; %>"
    "<ul><% for (item : menu) { %><li><a href=\"<%= item[\"url\"] %>\"><%= item[\"label\"] %></a></li><% } %></ul>\n"
    "<p>Visits: <%= stats[\"visits\"] %></p>\n"
    "<p>Found: <% for (word : search(\"a<b\", \"c&d\")) { %>[<%= word %>]<% } %></p>\n";
  int status = 1;

  GLTANG_Tree * tree = NULL;
  GLTANG_Program * program = NULL;
  GLTANG_ValueDescription * menu = build_menu();
  char * first = NULL;
  char * second = NULL;

  GLTANG_ParseError error;
  if (!menu || gltang_parse(source, GLTANG_PARSE_TEMPLATE, &error, &tree) != GLTANG_OK || gltang_compile(tree, "page.tang", &error, &program) != GLTANG_OK) {
    fprintf(stderr, "refused: %d:%d: %s\n", error.line, error.column, error.message);
    goto done;
  }

  /* The description is built once and read by two executions, each of which
   * makes its own copy: nothing one does to its copy is seen by the other. */
  first = render(program, menu);
  second = render(program, menu);
  if (!first || !second) {
    fprintf(stderr, "a page could not be rendered\n");
    goto done;
  }
  printf("--- first\n%s--- second\n%s", first, second);

  if (!strstr(first, "<li><a href=\"%2F\">Home</a></li>") || !strstr(first, "News &amp; events") || !strstr(first, "href=\"%2Fadmin+panel\">&lt;Admin&gt; area")
      || !strstr(first, "Visits: 41") || !strstr(second, "Visits: 42") || !strstr(first, "[a&lt;b][c&amp;d]")) {
    fprintf(stderr, "the pages are not what the template and the tags say\n");
    goto done;
  }
  status = 0;

done:
  gltang_buffer_free(first);
  gltang_buffer_free(second);
  gltang_program_release(program);
  gltang_tree_destroy(tree);
  gltang_value_description_release(menu);
  return status;
}
