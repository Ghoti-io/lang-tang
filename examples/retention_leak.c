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
 * Why is this value still alive? Ask the heap, from a paused program.
 *
 * A function builds a string, puts it in an array, keeps a local variable for
 * it, and loops. The host pauses the run with a fuel budget and reads the local
 * variable through the frame's scope interface (the way a debugger lists
 * locals): that is the string's address. It then raises the budget until the
 * program has cleared the local, and asks the heap's retention query about the
 * string again. Now nothing but the array holds it, and the answer says so: the
 * root is the guest stack, then the array, the array's storage, and the string,
 * each with the type's name and the byte offset of the slot that holds the next.
 *
 * The addresses in the answer are valid until the heap next collects or the
 * context next runs, so the example prints types and offsets and not addresses.
 * A program that wants a durable name for a step asks `grheap_id_of`.
 */

#include <ghoti.io/lang-tang/compile.h>
#include <ghoti.io/lang-tang/execution.h>
#include <ghoti.io/lang-tang/lang-tang.h>
#include <ghoti.io/lang-tang/program.h>
#include <ghoti.io/runtime-core/runtime-core.h>
#include <ghoti.io/runtime-heap/runtime-heap.h>

#include <stdio.h>
#include <string.h>

static const char * const source =
  "function work() {\n"
  "  s = \"leak\";\n"
  "  inner = s + s + s;\n"
  "  outer = [inner];\n"
  "  i = 0;\n"
  "  while (i < 3000) {\n"
  "    i = i + 1;\n"
  "  }\n"
  "  inner = null;\n"
  "  j = 0;\n"
  "  while (j < 3000) {\n"
  "    j = j + 1;\n"
  "  }\n"
  "  return outer;\n"
  "}\n"
  "work();\n";

/* The value word of the local variable `name` of the innermost frame that has a
 * local scope, read through the frame's scope interface. */
static int find_local(GRCORE_Context * context, const char * name, uint64_t * word) {
  GRCORE_FrameWalk walk;
  GRCORE_AbstractFrame frame;
  if (grcore_frame_walk_begin(context, &walk) != GRCORE_OK) {
    return 0;
  }
  while (grcore_frame_walk_next(&walk, &frame)) {
    GRCORE_ScopeInfo scope;
    if (grcore_frame_scope_count(&frame) == 0 || grcore_frame_scope(&frame, 0, &scope) != GRCORE_OK
        || scope.kind != GRCORE_SCOPE_LOCAL) {
      continue;
    }
    for (size_t i = 0; i < scope.variable_count; i++) {
      GRCORE_Variable v;
      if (grcore_frame_variable(&frame, 0, i, &v) == GRCORE_OK && strcmp(v.name, name) == 0) {
        *word = v.value;
        return 1;
      }
    }
  }
  return 0;
}

/* A pointer value has a zero tag; anything else is an integer, a boolean, ... */
static void * as_pointer(uint64_t word) {
  return (word & 0xF) == 0 && word != 0 ? (void *)(uintptr_t)word : NULL;
}

int main(void) {
  int status = 1;
  GLTANG_Tree * tree = NULL;
  GLTANG_Program * program = NULL;
  GLTANG_ParseError error;
  GRCORE_Group * group = NULL;
  GRCORE_Options * options = NULL;
  GRHEAP_Options * heap_options = NULL;
  GRCORE_Context * context = NULL;
  GRHEAP_Heap * heap = NULL;
  GLTANG_Execution * execution = NULL;
  GRHEAP_Retention * path = NULL;

  if (gltang_parse(source, GLTANG_PARSE_SCRIPT, &error, &tree) != GLTANG_OK
      || gltang_compile(tree, "leak.tang", &error, &program) != GLTANG_OK
      || grcore_group_create(NULL, NULL, &group) != GRCORE_OK
      || grcore_options_create(NULL, &options) != GRCORE_OK
      || grcore_options_set_fuel(options, 600) != GRCORE_OK
      || grheap_options_create(NULL, &heap_options) != GRHEAP_OK
      || gltang_heap_options_configure(heap_options) != GLTANG_OK
      || grcore_context_create(group, options, &context) != GRCORE_OK
      || grheap_heap_create(context, heap_options, &heap) != GRHEAP_OK
      || gltang_execution_create(context, program, &execution) != GLTANG_OK) {
    fprintf(stderr, "setup failed\n");
    goto done;
  }

  GRCORE_Outcome outcome;
  if (grcore_run(context, gltang_execution_entry, execution, &outcome) != GRCORE_OK
      || outcome != GRCORE_OUTCOME_PAUSED) {
    fprintf(stderr, "expected the run to pause in the first loop\n");
    goto done;
  }
  uint64_t word = 0;
  if (!find_local(context, "inner", &word) || as_pointer(word) == NULL) {
    fprintf(stderr, "expected the local `inner` to hold the string\n");
    goto done;
  }
  void * inner = as_pointer(word);

  /* Run on, a few hundred fuel at a time, until the program has cleared it. */
  for (;;) {
    if (!find_local(context, "inner", &word)) {
      fprintf(stderr, "the local disappeared\n");
      goto done;
    }
    if (as_pointer(word) == NULL) {
      break;
    }
    grcore_context_set_fuel(context, grcore_context_fuel_used(context) + 300);
    if (grcore_resume(context, &outcome) != GRCORE_OK || outcome != GRCORE_OUTCOME_PAUSED) {
      fprintf(stderr, "the run ended before the local was cleared\n");
      goto done;
    }
  }

  /* Why is the string still alive? */
  if (grheap_retention_path(heap, inner, NULL, &path) != GRHEAP_OK) {
    fprintf(stderr, "the query was refused\n");
    goto done;
  }
  grheap_retention_dump(path, stdout);
  GRHEAP_RetentionRoot root;
  if (!grheap_retention_retained(path) || grheap_retention_root(path, &root) != GRHEAP_OK
      || root.kind != GRHEAP_ROOT_CONTEXT_SOURCE || strcmp(root.source, "runtime-core.guest") != 0
      || grheap_retention_step_count(path) != 3) {
    fprintf(stderr, "expected a chain of three objects from the guest stack\n");
    goto done;
  }
  const char * want[3] = {"lang-tang array", "lang-tang array storage", "lang-tang string"};
  for (size_t i = 0; i < 3; i++) {
    GRHEAP_RetentionStep step;
    if (grheap_retention_step(path, i, &step) != GRHEAP_OK || strcmp(step.type->name, want[i]) != 0) {
      fprintf(stderr, "step %zu is not %s\n", i, want[i]);
      goto done;
    }
  }
  status = 0;

done:
  grheap_retention_release(path);
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
