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
 * Find the hot loop of a program by sampling.
 *
 * The host attaches runtime-core's sampling profiler to the context and starts
 * its timer at one millisecond. The timer thread does one thing: it posts a
 * request. The next poll, which is on the loop's back-edge, takes the sample: a
 * walk of the guest frames, counted by source location, as "self" (the
 * innermost frame was here) and "inclusive" (this location was on the stack).
 * Nothing about the program changes, and no `tang` flag turns this on; a host
 * registers the profiler, as this one does.
 *
 * The profile is biased to polls: a sample is taken at the first poll after the
 * request, so the time between two polls is charged to the poll that ends it.
 * In a loop that is the loop's own back-edge, which is what the example shows.
 * The profile is the same with the JIT off and on, because a poll identity is
 * the same on every tier; the example runs both and says so.
 *
 * It runs the loop longer until the profile has a floor of samples, instead of
 * asserting on a handful. The library may be built without the JIT (`JIT=no`);
 * then both runs are the interpreter's and the example still passes.
 */

#include <ghoti.io/lang-tang/compile.h>
#include <ghoti.io/lang-tang/execution.h>
#include <ghoti.io/lang-tang/lang-tang.h>
#include <ghoti.io/lang-tang/program.h>
#include <ghoti.io/runtime-core/runtime-core.h>
#include <ghoti.io/runtime-heap/runtime-heap.h>

#include <stdio.h>
#include <string.h>

#define FLOOR_SAMPLES 40u

/* A short prologue (lines 1 to 2), then one loop (lines 5 to 8). */
static const char * const source_format =
  "x = 1;\n"
  "function spin(n) {\n"
  "  i = 0;\n"
  "  t = 0;\n"
  "  while (i < n) {\n"
  "    t = t + i;\n"
  "    i = i + 1;\n"
  "  }\n"
  "  return t;\n"
  "}\n"
  "print(spin(%llu));\n";

typedef struct Profile {
  GRCORE_ProfileTotals totals;
  GRCORE_ProfileEntry entries[16];
  char files[16][64]; /* the entries' file names, copied: see below */
  size_t count;
  unsigned long long iterations;
  uint64_t in_loop; /* self samples on lines 5 to 8 */
} Profile;

static int profile_once(unsigned long long n, unsigned threshold, Profile * out) {
  int status = 1;
  char source[512];
  GLTANG_Tree * tree = NULL;
  GLTANG_Program * program = NULL;
  GLTANG_ParseError error;
  GRCORE_Group * group = NULL;
  GRCORE_Options * options = NULL;
  GRHEAP_Options * heap_options = NULL;
  GRCORE_Context * context = NULL;
  GRHEAP_Heap * heap = NULL;
  GLTANG_Execution * execution = NULL;
  GRCORE_Profiler * profiler = NULL;

  snprintf(source, sizeof(source), source_format, n);
  if (gltang_parse(source, GLTANG_PARSE_SCRIPT, &error, &tree) != GLTANG_OK
      || gltang_compile(tree, "hot.tang", &error, &program) != GLTANG_OK
      || grcore_group_create(NULL, NULL, &group) != GRCORE_OK
      || grcore_options_create(NULL, &options) != GRCORE_OK
      || grheap_options_create(NULL, &heap_options) != GRHEAP_OK
      || gltang_heap_options_configure(heap_options) != GLTANG_OK
      || grcore_context_create(group, options, &context) != GRCORE_OK
      || grheap_heap_create(context, heap_options, &heap) != GRHEAP_OK
      || gltang_execution_create(context, program, &execution) != GLTANG_OK) {
    fprintf(stderr, "setup failed\n");
    goto done;
  }
  GLTANG_Result set = gltang_execution_set_jit_threshold(execution, threshold);
  if (set != GLTANG_OK && !(set == GLTANG_ERR_UNSUPPORTED && !gltang_jit_built())) {
    fprintf(stderr, "could not set the threshold\n");
    goto done;
  }
  if (grcore_profiler_attach(context, 0, &profiler) != GRCORE_OK
      || grcore_profiler_timer_start(profiler, 1000) != GRCORE_OK) {
    fprintf(stderr, "could not start the profiler\n");
    goto done;
  }
  GRCORE_Outcome outcome;
  if (grcore_run(context, gltang_execution_entry, execution, &outcome) != GRCORE_OK
      || outcome != GRCORE_OUTCOME_FINISHED) {
    fprintf(stderr, "the run did not finish\n");
    goto done;
  }
  grcore_profiler_timer_stop(profiler);
  if (grcore_profiler_report(profiler, out->entries, 16, &out->count, &out->totals) != GRCORE_OK) {
    goto done;
  }
  out->iterations = n;
  out->in_loop = 0;
  for (size_t i = 0; i < out->count; i++) {
    /* An entry's `file` is the pointer the engine gave, which is the program's
     * string: it lives as long as the program does, and the program is released
     * below. A host that keeps a profile past that copies the names. */
    snprintf(out->files[i], sizeof(out->files[i]), "%s", out->entries[i].file ? out->entries[i].file : "");
    out->entries[i].file = out->files[i];
    if (strcmp(out->entries[i].file, "hot.tang") == 0 && out->entries[i].line >= 5 && out->entries[i].line <= 8) {
      out->in_loop += out->entries[i].self;
    }
  }
  status = 0;

done:
  if (context) {
    grcore_context_destroy(context); /* stops the timer if it is still running */
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

/* Runs the program at a threshold, with a longer loop each time until the
 * profile has enough samples to say something. */
static int profile_until_enough(unsigned threshold, Profile * out) {
  for (unsigned long long n = 200000; n <= 400000000ull; n *= 4) {
    if (profile_once(n, threshold, out)) {
      return 1;
    }
    if (out->totals.samples >= FLOOR_SAMPLES) {
      return 0;
    }
  }
  fprintf(stderr, "never reached %u samples\n", FLOOR_SAMPLES);
  return 1;
}

static void print_profile(const char * title, const Profile * p) {
  printf("%s: %llu iterations, %llu samples, %llu in the loop's lines 5 to 8\n", title, p->iterations,
      (unsigned long long)p->totals.samples, (unsigned long long)p->in_loop);
  for (size_t i = 0; i < p->count && i < 5; i++) {
    printf("  %s:%d  self %llu  inclusive %llu\n", p->entries[i].file, p->entries[i].line,
        (unsigned long long)p->entries[i].self, (unsigned long long)p->entries[i].inclusive);
  }
}

int main(void) {
  Profile interpreted, compiled;
  memset(&interpreted, 0, sizeof(interpreted));
  memset(&compiled, 0, sizeof(compiled));
  if (profile_until_enough(0, &interpreted) || profile_until_enough(1, &compiled)) {
    return 1;
  }
  print_profile("interpreted", &interpreted);
  print_profile("JIT at threshold 1", &compiled);
  /* Most samples are in the loop, on both tiers. */
  if (interpreted.in_loop * 2 <= interpreted.totals.samples || compiled.in_loop * 2 <= compiled.totals.samples) {
    fprintf(stderr, "expected most samples in the loop\n");
    return 1;
  }
  /* The same lines are hot on both: the top entry is a line of the loop. */
  if (interpreted.count == 0 || compiled.count == 0 || interpreted.entries[0].line < 5 || interpreted.entries[0].line > 8
      || compiled.entries[0].line < 5 || compiled.entries[0].line > 8) {
    fprintf(stderr, "expected the hottest line to be in the loop on both tiers\n");
    return 1;
  }
  printf("the loop is where the time goes, on both tiers\n");
  return 0;
}
