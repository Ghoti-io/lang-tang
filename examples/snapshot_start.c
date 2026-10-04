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
 * Start many contexts from one warmed-up snapshot.
 *
 * A host that serves many requests with the same program does the same
 * start-up work for each context: a table is built, a cache is filled. That is
 * what a snapshot is for. This program runs a program's prologue once, in a
 * context that is paused part way through it, freezes that context into a snapshot,
 * and then makes several fresh contexts, restoring each from the snapshot
 * instead of running the prologue again. Each finishes the request, and the
 * output of every one is what a context that ran the whole program from scratch
 * prints.
 *
 * The snapshot is immutable and holds no address, so it could be restored on
 * other threads, or after the context it came from was gone; here the
 * origin context is destroyed before the first restore to show it.
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

/* The prologue builds a table; the rest of the program uses it. The loop at the
 * end is where the origin context is paused. */
static const char source[] =
  "table = [];\n"
  "for (i = 0; i < 5000; i += 1) { table[i] = i * 3; }\n"
  "total = 0;\n"
  "for (k = 0; k < 200; k += 1) { total += table[k * 20]; }\n"
  "print(total);\n";

/* A context for the program with the given fuel (GRCORE_UNLIMITED for none). */
typedef struct {
  GRCORE_Group * group;
  GRCORE_Context * context;
  GRHEAP_Heap * heap;
  GLTANG_Execution * execution;
} Runtime;

static int open_runtime(Runtime * rt, GLTANG_Program * program, uint64_t fuel) {
  GRCORE_Options * options = NULL;
  GRHEAP_Options * heap_options = NULL;
  memset(rt, 0, sizeof(*rt));
  if (grcore_group_create(NULL, NULL, &rt->group) != GRCORE_OK
      || grcore_options_create(NULL, &options) != GRCORE_OK
      || grheap_options_create(NULL, &heap_options) != GRHEAP_OK) {
    return 1;
  }
  grcore_options_set_fuel(options, fuel);
  int bad = gltang_heap_options_configure(heap_options) != GLTANG_OK
      || grcore_context_create(rt->group, options, &rt->context) != GRCORE_OK
      || grheap_heap_create(rt->context, heap_options, &rt->heap) != GRHEAP_OK
      || gltang_execution_create(rt->context, program, &rt->execution) != GLTANG_OK;
  grheap_options_destroy(heap_options);
  grcore_options_destroy(options);
  return bad;
}

static void close_runtime(Runtime * rt) {
  if (rt->context) {
    grcore_context_destroy(rt->context);
  }
  if (rt->group) {
    grcore_group_destroy(rt->group);
  }
}

int main(void) {
  int status = 1;
  GLTANG_Tree * tree = NULL;
  GLTANG_Program * program = NULL;
  GLTANG_Snapshot * snapshot = NULL;
  GLTANG_ParseError error;
  Runtime origin, scratch;
  memset(&origin, 0, sizeof(origin));
  memset(&scratch, 0, sizeof(scratch));

  if (gltang_parse(source, GLTANG_PARSE_SCRIPT, &error, &tree) != GLTANG_OK
      || gltang_compile(tree, "report.tang", &error, &program) != GLTANG_OK) {
    fprintf(stderr, "refused: %d:%d: %s\n", error.line, error.column, error.message);
    goto done;
  }

  /* The reference: the whole program, from scratch. */
  GRCORE_Outcome outcome;
  if (open_runtime(&scratch, program, GRCORE_UNLIMITED) != 0
      || grcore_run(scratch.context, gltang_execution_entry, scratch.execution, &outcome) != GRCORE_OK
      || outcome != GRCORE_OUTCOME_FINISHED) {
    fprintf(stderr, "the reference run failed\n");
    goto done;
  }
  size_t length = 0;
  const char * expected = gltang_execution_output_raw(scratch.execution, &length);
  char reference[64];
  snprintf(reference, sizeof(reference), "%.*s", (int)length, expected);
  uint64_t whole_run_fuel = grcore_context_fuel_used(scratch.context);

  /* The prologue, once: run a context until it has spent a third of the
   * program's fuel, which lands part way through the work and not at an end of
   * anything: a snapshot is of wherever a pause is. */
  if (open_runtime(&origin, program, whole_run_fuel / 3) != 0
      || grcore_run(origin.context, gltang_execution_entry, origin.execution, &outcome) != GRCORE_OK
      || outcome != GRCORE_OUTCOME_PAUSED) {
    fprintf(stderr, "the origin context did not pause\n");
    goto done;
  }
  if (gltang_snapshot_take(origin.execution, &snapshot) != GLTANG_OK) {
    fprintf(stderr, "the snapshot was refused\n");
    goto done;
  }
  printf("snapshot taken at %llu of %llu units of fuel: %zu bytes\n",
      (unsigned long long)grcore_context_fuel_used(origin.context), (unsigned long long)whole_run_fuel,
      gltang_snapshot_size(snapshot));
  close_runtime(&origin);
  memset(&origin, 0, sizeof(origin)); /* the snapshot does not need it */

  /* Several fresh contexts, started from the snapshot. */
  for (int request = 1; request <= 3; ++request) {
    Runtime rt;
    if (open_runtime(&rt, program, GRCORE_UNLIMITED) != 0) {
      fprintf(stderr, "could not build a context\n");
      goto done;
    }
    /* The libraries, budgets and so on are the host's to give, exactly as for a
     * fresh run; this program needs none. Then the restore. */
    if (gltang_snapshot_restore(rt.execution, snapshot) != GLTANG_OK
        || grcore_resume(rt.context, &outcome) != GRCORE_OK
        || outcome != GRCORE_OUTCOME_FINISHED) {
      fprintf(stderr, "request %d did not finish from the snapshot\n", request);
      close_runtime(&rt);
      goto done;
    }
    size_t n = 0;
    const char * out = gltang_execution_output_raw(rt.execution, &n);
    printf("request %d, from the snapshot: %.*s (fuel used here: %llu)\n", request, (int)n, out,
        (unsigned long long)grcore_context_fuel_used(rt.context));
    int same = n == strlen(reference) && !strncmp(out, reference, n);
    close_runtime(&rt);
    if (!same) {
      fprintf(stderr, "the output differs from a run from scratch (%s)\n", reference);
      goto done;
    }
  }
  printf("every request printed what a run from scratch prints: %s\n", reference);
  status = 0;

done:
  gltang_snapshot_release(snapshot);
  close_runtime(&origin);
  close_runtime(&scratch);
  gltang_program_release(program);
  gltang_tree_destroy(tree);
  return status;
}
