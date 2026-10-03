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
 * Stop a runaway program, say where it was, and decide what to do next.
 *
 * A context given a fuel budget pauses when the budget is spent. The pause is
 * not a failure: the program's state is intact, the host can read where it
 * stopped, and it can raise the budget and resume or unwind the run. This
 * program pauses a loop that never ends, prints the file and line it stopped
 * on, raises the fuel once to show that the run goes on from the same place,
 * and then gives up on it, which unwinds every frame and reports the limit.
 */

#include <ghoti.io/lang-tang/compile.h>
#include <ghoti.io/lang-tang/execution.h>
#include <ghoti.io/lang-tang/lang-tang.h>
#include <ghoti.io/lang-tang/program.h>
#include <ghoti.io/lang-tang/value.h>
#include <ghoti.io/runtime-core/runtime-core.h>
#include <ghoti.io/runtime-heap/runtime-heap.h>

#include <stdio.h>

int main(void) {
  const char * source =
    "count = 0;\n"
    "while (true) {\n"
    "  count = count + 1;\n"
    "}\n";
  int status = 1;

  GLTANG_Tree * tree = NULL;
  GLTANG_Program * program = NULL;
  GRCORE_Group * group = NULL;
  GRCORE_Options * options = NULL;
  GRHEAP_Options * heap_options = NULL;
  GRCORE_Context * context = NULL;
  GRHEAP_Heap * heap = NULL;
  GLTANG_Execution * execution = NULL;

  GLTANG_ParseError error;
  if (gltang_parse(source, GLTANG_PARSE_SCRIPT, &error, &tree) != GLTANG_OK
      || gltang_compile(tree, "runaway.tang", &error, &program) != GLTANG_OK) {
    fprintf(stderr, "refused: %d:%d: %s\n", error.line, error.column, error.message);
    goto done;
  }

  if (grcore_group_create(NULL, NULL, &group) != GRCORE_OK
      || grcore_options_create(NULL, &options) != GRCORE_OK
      || grheap_options_create(NULL, &heap_options) != GRHEAP_OK) {
    fprintf(stderr, "setup: out of memory\n");
    goto done;
  }
  grcore_options_set_fuel(options, 5000);
  if (gltang_heap_options_configure(heap_options) != GLTANG_OK
      || grcore_context_create(group, options, &context) != GRCORE_OK
      || grheap_heap_create(context, heap_options, &heap) != GRHEAP_OK
      || gltang_execution_create(context, program, &execution) != GLTANG_OK) {
    fprintf(stderr, "setup: could not build the context\n");
    goto done;
  }

  /* The first run stops when the fuel is spent. */
  GRCORE_Outcome outcome;
  if (grcore_run(context, gltang_execution_entry, execution, &outcome) != GRCORE_OK
      || outcome != GRCORE_OUTCOME_PAUSED) {
    fprintf(stderr, "expected the loop to be paused\n");
    goto done;
  }
  GRCORE_Location where = grcore_context_pause_location(context);
  printf("paused at %s:%d after %llu units of fuel\n", where.file, where.line,
      (unsigned long long)grcore_context_fuel_used(context));
  if (where.line != 2) {
    fprintf(stderr, "the loop is on line 2\n");
    goto done;
  }

  /* More fuel and resume: the loop carries on from the place it stopped. */
  uint64_t first_stop = grcore_context_fuel_used(context);
  grcore_context_set_fuel(context, first_stop + 5000);
  if (grcore_resume(context, &outcome) != GRCORE_OK || outcome != GRCORE_OUTCOME_PAUSED) {
    fprintf(stderr, "expected a second pause\n");
    goto done;
  }
  printf("resumed and paused again after %llu more units\n",
      (unsigned long long)(grcore_context_fuel_used(context) - first_stop));

  /* Give up: ask for an unwind, and the next resume pops every frame and
   * reports why. */
  if (grcore_context_terminate(context) != GRCORE_OK
      || grcore_resume(context, &outcome) != GRCORE_ERR_LIMIT) {
    fprintf(stderr, "expected the unwind to report the limit\n");
    goto done;
  }
  printf("unwound %llu frame(s): %s\n",
      (unsigned long long)gltang_execution_unwound_frames(execution),
      grcore_result_string(grcore_context_unwind_result(context)));
  status = 0;

done:
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
