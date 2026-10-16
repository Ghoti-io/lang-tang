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
 * The rest of the host API: a context per request, a library every program can
 * use, and output that streams.
 *
 *  - `whoami()` is one native registered once; it reads the request it is
 *    answering from the execution's host context, so the same function serves
 *    every request;
 *  - `site` is a library the host puts in the language layer once, for every
 *    program and every execution in the process;
 *  - the output sink receives each print as it finishes, with the encoding tag
 *    of every segment, and writes it to the page as it goes: what the program
 *    prints from a user's text is escaped, the template's own text is not;
 *  - `gltang_unicode_string_render` and `html_encode` are the same encodings
 *    for the host's own text.
 */

#include <ghoti.io/lang-tang/compile.h>
#include <ghoti.io/lang-tang/execution.h>
#include <ghoti.io/lang-tang/handle.h>
#include <ghoti.io/lang-tang/lang-tang.h>
#include <ghoti.io/lang-tang/language.h>
#include <ghoti.io/lang-tang/library.h>
#include <ghoti.io/lang-tang/program.h>
#include <ghoti.io/lang-tang/unicodeString.h>
#include <ghoti.io/lang-tang/value.h>
#include <ghoti.io/cutil/memory.h>
#include <ghoti.io/runtime-core/runtime-core.h>
#include <ghoti.io/runtime-heap/runtime-heap.h>

#include <stdio.h>
#include <string.h>

typedef struct Request {
  const char * user;   /* what the program is told about the visitor: not trusted */
  char page[4096];     /* the page, as the sink builds it */
  size_t length;
  int segments;
} Request;

/* One native for every request: the request is the execution's host context. */
static bool whoami(GLTANG_NativeCall * call, void * user) {
  (void)user;
  const Request * request = gltang_call_host_context(call);
  if (!request) {
    return false;
  }
  /* The visitor's name is untrusted: it is an HTML-tagged string. */
  gltang_call_return_string(call, request->user, strlen(request->user), GLTANG_UNICODE_STRING_TYPE_HTML);
  return true;
}

/* The sink gets each print whole, with the tag of each segment; the page takes it encoded. */
static bool to_page(void * user, const char * bytes, size_t length, GLTANG_String_Type type) {
  Request * request = user;
  GLTANG_Unicode_String * segment = gltang_unicode_string_create(bytes, length, type);
  if (!segment) {
    return false;
  }
  GLTANG_Unicode_Rendered_String encoded = gltang_unicode_string_render(segment);
  gltang_unicode_string_destroy(segment);
  if (!encoded.buffer || request->length + encoded.length >= sizeof request->page) {
    gcu_free(encoded.buffer);
    return false;   /* a full page refuses: that print fails, the program goes on */
  }
  memcpy(request->page + request->length, encoded.buffer, encoded.length);
  request->length += encoded.length;
  request->page[request->length] = '\0';
  ++request->segments;
  gcu_free(encoded.buffer);
  return true;
}

static int serve(GLTANG_Program * program, Request * request) {
  GRCORE_Group * group = NULL;
  GRCORE_Options * options = NULL;
  GRHEAP_Options * heap_options = NULL;
  GRCORE_Context * context = NULL;
  GRHEAP_Heap * heap = NULL;
  GLTANG_Execution * execution = NULL;
  GLTANG_Library * root = NULL;
  int status = 1;

  if (grcore_group_create(NULL, NULL, &group) != GRCORE_OK || grcore_options_create(NULL, &options) != GRCORE_OK || grheap_options_create(NULL, &heap_options) != GRHEAP_OK) {
    goto done;
  }
  /* The language layer is read when the execution is created, so it is set before. */
  if (gltang_heap_options_configure(heap_options) != GLTANG_OK || grcore_context_create(group, options, &context) != GRCORE_OK
      || grheap_heap_create(context, heap_options, &heap) != GRHEAP_OK || gltang_execution_create(context, program, &execution) != GLTANG_OK) {
    goto done;
  }
  if (gltang_library_create(NULL, &root) != GLTANG_OK || gltang_library_add_native(root, "whoami", whoami, NULL) != GLTANG_OK
      || gltang_execution_set_libraries(execution, root) != GLTANG_OK || gltang_execution_set_host_context(execution, request) != GLTANG_OK
      || gltang_execution_set_output_sink(execution, to_page, request) != GLTANG_OK) {
    goto done;
  }
  GRCORE_Outcome outcome;
  if (grcore_run(context, gltang_execution_entry, execution, &outcome) != GRCORE_OK || outcome != GRCORE_OUTCOME_FINISHED) {
    goto done;
  }
  status = 0;

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
  return status;
}

int main(void) {
  const char * source =
    "<% use whoami; use site; %><h1><%= site.title %></h1>\n"
    "<p>Hello, <%= whoami() %>!</p>\n";
  int status = 1;

  /* Once, for the whole process: the library every program finds after its own. */
  GLTANG_Library * layer = NULL;
  GLTANG_Library * site = NULL;
  GLTANG_Tree * tree = NULL;
  GLTANG_Program * program = NULL;
  if (gltang_library_create(NULL, &layer) != GLTANG_OK || gltang_library_create("site", &site) != GLTANG_OK
      || gltang_library_add_string(site, "title", "Tom & Jerry's", 13, GLTANG_UNICODE_STRING_TYPE_HTML) != GLTANG_OK
      || gltang_library_add_library(layer, site) != GLTANG_OK || gltang_language_set_library(layer) != GLTANG_OK) {
    fprintf(stderr, "the language layer could not be made\n");
    goto done;
  }

  GLTANG_ParseError error;
  if (gltang_parse(source, GLTANG_PARSE_TEMPLATE, &error, &tree) != GLTANG_OK || gltang_compile(tree, "page.tang", &error, &program) != GLTANG_OK) {
    fprintf(stderr, "refused: %d:%d: %s\n", error.line, error.column, error.message);
    goto done;
  }

  /* Two requests, one program, one native: each execution reads its own context. */
  Request first = {.user = "Ann <script>alert(1)</script>"};
  Request second = {.user = "Bob"};
  if (serve(program, &first) != 0 || serve(program, &second) != 0) {
    fprintf(stderr, "a request could not be served\n");
    goto done;
  }
  printf("%s%s", first.page, second.page);

  /* The visitor's text was escaped as it streamed; the template's own markup was not;
   * the layer's title reached both pages; the two contexts did not mix. */
  if (!strstr(first.page, "Hello, Ann &lt;script&gt;alert(1)&lt;/script&gt;!") || strstr(first.page, "<script>") || !strstr(first.page, "<h1>Tom &amp; Jerry's</h1>")
      || !strstr(second.page, "Hello, Bob!") || strstr(second.page, "Ann") || first.segments < 4) {
    fprintf(stderr, "the pages are not what the tags and the contexts say\n");
    goto done;
  }
  GLTANG_Unicode_Rendered_String helper = gltang_unicode_string_html_encode("<b>", 3);
  status = helper.buffer && strcmp(helper.buffer, "&lt;b&gt;") == 0 ? 0 : 1;
  gcu_free(helper.buffer);

done:
  gltang_program_release(program);
  gltang_tree_destroy(tree);
  (void)gltang_language_set_library(NULL);
  gltang_library_release(site);
  gltang_library_release(layer);
  return status;
}
