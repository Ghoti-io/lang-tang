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
 * A host function that calls a Tang function: `map(values, fn)`.
 *
 * A native function is opaque: it runs inside the run and cannot call guest code
 * or pause. A *resumable* native asks instead. It calls gltang_call_guest and
 * returns at once; the engine makes the call from its own loop, so a pause inside
 * `fn` is an ordinary pause (here the run is paused every few units of fuel and
 * resumed, and the output is the same), and then calls the native's continuation
 * with the value. This one keeps the results so far in its *state*, an array it
 * asks for with the next element and hands back to itself, and answers the array
 * when the last element is done.
 *
 *  - the continuation table is given when the native is marked resumable
 *    (gltang_library_mark_resumable); a request names an entry by its index;
 *  - nothing of the first call survives the request: the handles of the entry are
 *    gone, and the continuation reads its arguments again (gltang_call_arg) and
 *    gets the state and the result as handles of its own;
 *  - a value `fn` answers that is an error is an ordinary value to the
 *    continuation, which here answers it as the map's own value.
 */

#include <ghoti.io/lang-tang/compile.h>
#include <ghoti.io/lang-tang/execution.h>
#include <ghoti.io/lang-tang/handle.h>
#include <ghoti.io/lang-tang/lang-tang.h>
#include <ghoti.io/lang-tang/library.h>
#include <ghoti.io/lang-tang/program.h>
#include <ghoti.io/lang-tang/type.h>
#include <ghoti.io/lang-tang/value.h>
#include <ghoti.io/runtime-core/runtime-core.h>
#include <ghoti.io/runtime-heap/runtime-heap.h>

#include <stdio.h>
#include <string.h>

/* Asks for fn(values[index]) and for the continuation to be called with the state. */
static bool ask(GLTANG_NativeCall * call, size_t index, GLTANG_Handle state) {
  GLTANG_Handle values, fn, element;
  if (gltang_call_arg(call, 0, &values) != GLTANG_OK || gltang_call_arg(call, 1, &fn) != GLTANG_OK
      || gltang_handle_array_get(call, values, index, &element) != GLTANG_OK) {
    return false;
  }
  return gltang_call_guest(call, fn, &element, 1, 0, state) == GLTANG_OK;
}

/* The native: starts with an empty state, or answers an empty array for no values. */
static bool map_values(GLTANG_NativeCall * call, void * user) {
  (void)user;
  GLTANG_Handle values, state;
  if (gltang_call_arg(call, 0, &values) != GLTANG_OK || gltang_handle_make_array(call, 0, &state) != GLTANG_OK) {
    return false;
  }
  if (gltang_handle_array_length(call, values) == 0) {
    gltang_call_return_handle(call, state);
    return true;
  }
  return ask(call, 0, state);  /* return true after a request, and touch nothing else */
}

/* Called with fn(values[i])'s value once the engine has run it. */
static GLTANG_OpStatus map_continue(GLTANG_NativeCall * call, void * user, GLTANG_Handle state, GLTANG_Handle result) {
  (void)user;
  GLTANG_Handle values;
  if (gltang_call_arg(call, 0, &values) != GLTANG_OK) {
    return GLTANG_OP_ERROR;
  }
  if (gltang_handle_kind(call, result) == GLTANG_KIND_ERROR) {
    gltang_call_return_handle(call, result);  /* an error value is the map's value */
    return GLTANG_OP_OK;
  }
  if (gltang_handle_array_push(call, state, result) != GLTANG_OK) {
    return GLTANG_OP_ERROR;
  }
  size_t done = gltang_handle_array_length(call, state);
  if (done == gltang_handle_array_length(call, values)) {
    gltang_call_return_handle(call, state);
    return GLTANG_OP_OK;
  }
  return ask(call, done, state) ? GLTANG_OP_PENDING : GLTANG_OP_ERROR;
}

int main(void) {
  int status = 1;
  GLTANG_Tree * tree = NULL;
  GLTANG_Program * program = NULL;
  GRCORE_Group * group = NULL;
  GRCORE_Options * options = NULL;
  GRHEAP_Options * heap_options = NULL;
  GRCORE_Context * context = NULL;
  GRHEAP_Heap * heap = NULL;
  GLTANG_Execution * execution = NULL;
  GLTANG_Library * library = NULL;

  const char * source =
    "use map;\n"
    "function bump(x) { k = 0; while (k < 20) { k = k + 1; } return x * 10 + k; }\n"
    "r = map([1, 2, 3], bump);\n"
    "print(r[0]); print(\" \"); print(r[1]); print(\" \"); print(r[2]);\n";
  GLTANG_ParseError error;
  if (gltang_parse(source, GLTANG_PARSE_SCRIPT, &error, &tree) != GLTANG_OK || gltang_compile(tree, "map.tang", &error, &program) != GLTANG_OK) {
    fprintf(stderr, "refused: %d:%d: %s\n", error.line, error.column, error.message);
    goto done;
  }
  /* A budget small enough that the run pauses inside `bump`; the loop below raises it and resumes. */
  if (grcore_group_create(NULL, NULL, &group) != GRCORE_OK || grcore_options_create(NULL, &options) != GRCORE_OK || grheap_options_create(NULL, &heap_options) != GRHEAP_OK
      || grcore_options_set_fuel(options, 60) != GRCORE_OK
      || gltang_heap_options_configure(heap_options) != GLTANG_OK || grcore_context_create(group, options, &context) != GRCORE_OK
      || grheap_heap_create(context, heap_options, &heap) != GRHEAP_OK || gltang_execution_create(context, program, &execution) != GLTANG_OK) {
    fprintf(stderr, "the runtime could not be made\n");
    goto done;
  }
  static const GLTANG_Continuation table[] = {{"map_continue", map_continue}};
  if (gltang_library_create(NULL, &library) != GLTANG_OK || gltang_library_add_native(library, "map", map_values, NULL) != GLTANG_OK
      || gltang_library_mark_resumable(library, "map", table, 1) != GLTANG_OK || gltang_execution_set_libraries(execution, library) != GLTANG_OK) {
    fprintf(stderr, "the program's library could not be attached\n");
    goto done;
  }
  GRCORE_Outcome outcome;
  unsigned pauses = 0;
  GRCORE_Result ran = grcore_run(context, gltang_execution_entry, execution, &outcome);
  while (ran == GRCORE_OK && outcome == GRCORE_OUTCOME_PAUSED && pauses < 1000u) {
    ++pauses;
    grcore_context_set_fuel(context, grcore_context_fuel_used(context) + 60u);
    ran = grcore_resume(context, &outcome);
  }
  if (ran != GRCORE_OK || outcome != GRCORE_OUTCOME_FINISHED) {
    fprintf(stderr, "the run did not finish\n");
    goto done;
  }
  size_t length = 0;
  const char * output = gltang_execution_output_raw(execution, &length);
  printf("%.*s (paused %u times)\n", (int)length, output, pauses);
  const char * expected = "30 40 50";  /* x * 10 + 20 */
  if (length != strlen(expected) || memcmp(output, expected, length) != 0 || pauses == 0) {
    fprintf(stderr, "the map did not compute what it should\n");
    goto done;
  }
  status = 0;

done:
  gltang_library_release(library);
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
