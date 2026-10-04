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
 * See a function tier up.
 *
 * A function that is called, or loops, enough is compiled to machine code, and
 * the interpreter enters the compiled code the next time the function starts.
 * Nothing the program does is different: this example runs one loop twice, once
 * with the JIT off and once with every function tiering up at its first poll,
 * and checks that the output, the result and the fuel are the same. It then
 * reads the counters the execution keeps (functions compiled, entries into
 * compiled code, how often compiled code left for the interpreter) and says what
 * they were.
 *
 * The library may be built without the JIT (`JIT=no`); then `gltang_jit_built`
 * is false, the threshold is refused with ::GLTANG_ERR_UNSUPPORTED and the
 * counters are zero, and the two runs are both the interpreter's. This program
 * runs in both builds and passes in both.
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

/* runtime-jit compiles for Linux x86-64, Linux arm64 and Windows x86-64 and
 * for nothing else (its backend.h), whatever the library was built with. */
#if ((defined(__x86_64__) || defined(__aarch64__)) && defined(__linux__)) || \
    (defined(_WIN64) && defined(__x86_64__))
#define JIT_BACKEND_EXISTS 1
#else
#define JIT_BACKEND_EXISTS 0
#endif

typedef struct Run {
  char output[64];
  long long result;
  unsigned long long fuel;
  GLTANG_JitStats stats;
} Run;

/* Runs the program once with the given threshold (0: never tier up). */
static int run_once(GLTANG_Program * program, unsigned threshold, Run * out) {
  int status = 1;
  GRCORE_Group * group = NULL;
  GRCORE_Options * options = NULL;
  GRHEAP_Options * heap_options = NULL;
  GRCORE_Context * context = NULL;
  GRHEAP_Heap * heap = NULL;
  GLTANG_Execution * execution = NULL;

  if (grcore_group_create(NULL, NULL, &group) != GRCORE_OK
      || grcore_options_create(NULL, &options) != GRCORE_OK
      || grheap_options_create(NULL, &heap_options) != GRHEAP_OK
      || gltang_heap_options_configure(heap_options) != GLTANG_OK
      || grcore_context_create(group, options, &context) != GRCORE_OK
      || grheap_heap_create(context, heap_options, &heap) != GRHEAP_OK
      || gltang_execution_create(context, program, &execution) != GLTANG_OK) {
    fprintf(stderr, "setup: could not build the context\n");
    goto done;
  }

  /* The setter is the whole host API of the JIT besides the counters. A build
   * without the JIT refuses it, and that is not an error here. */
  GLTANG_Result set = gltang_execution_set_jit_threshold(execution, threshold);
  if (set != GLTANG_OK && !(set == GLTANG_ERR_UNSUPPORTED && !gltang_jit_built())) {
    fprintf(stderr, "could not set the threshold: %s\n", gltang_result_string(set));
    goto done;
  }

  GRCORE_Outcome outcome;
  if (grcore_run(context, gltang_execution_entry, execution, &outcome) != GRCORE_OK || outcome != GRCORE_OUTCOME_FINISHED) {
    fprintf(stderr, "the run did not finish\n");
    goto done;
  }
  size_t length = 0;
  const char * raw = gltang_execution_output_raw(execution, &length);
  snprintf(out->output, sizeof(out->output), "%.*s", (int)length, raw);
  out->result = (long long)gltang_execution_result_integer(execution);
  out->fuel = (unsigned long long)grcore_context_fuel_used(context);
  if (gltang_execution_jit_stats(execution, &out->stats) != GLTANG_OK) {
    goto done;
  }
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
  return status;
}

int main(void) {
  const char * source =
    "function count(n) {\n"
    "  total = 0;\n"
    "  i = 0;\n"
    "  while (i < n) {\n"
    "    total = total + i * 2;\n"
    "    i = i + 1;\n"
    "  }\n"
    "  return total;\n"
    "}\n"
    "print(count(10));\n"
    "print(\",\");\n"
    "print(count(10000));\n"
    "count(1000);\n";
  int status = 1;
  GLTANG_Tree * tree = NULL;
  GLTANG_Program * program = NULL;
  GLTANG_ParseError error;
  if (gltang_parse(source, GLTANG_PARSE_SCRIPT, &error, &tree) != GLTANG_OK
      || gltang_compile(tree, "count.tang", &error, &program) != GLTANG_OK) {
    fprintf(stderr, "refused: %d:%d: %s\n", error.line, error.column, error.message);
    goto done;
  }

  Run interpreter, compiled;
  memset(&interpreter, 0, sizeof(interpreter));
  memset(&compiled, 0, sizeof(compiled));
  if (run_once(program, 0, &interpreter) || run_once(program, 1, &compiled)) {
    goto done;
  }

  printf("output: %s, result: %lld, fuel: %llu\n", compiled.output, compiled.result, compiled.fuel);
  if (strcmp(interpreter.output, compiled.output) != 0 || interpreter.result != compiled.result || interpreter.fuel != compiled.fuel) {
    fprintf(stderr, "the JIT changed what the program did: \"%s\" %lld %llu against \"%s\" %lld %llu\n", interpreter.output,
        interpreter.result, interpreter.fuel, compiled.output, compiled.result, compiled.fuel);
    goto done;
  }
  printf("same output, result and fuel with the JIT off\n");
  if (gltang_jit_built() && !JIT_BACKEND_EXISTS) {
    /* The JIT is built but runtime-jit has no backend for this target, so
     * nothing tiers up: the two runs above were both the interpreter's and
     * agree, which is all that can be shown. Exit status 77 is "skipped": the
     * Makefile counts it and does not fail. */
    printf("SKIP: the JIT is built but this target has no native code backend, so no function tiers up\n");
    status = 77;
    goto done;
  }
  if (gltang_jit_built()) {
    printf("compiled %llu function(s); entered compiled code %llu time(s), %llu call(s) returned from it, and it left for the interpreter %llu time(s)\n",
        (unsigned long long)compiled.stats.functions_compiled, (unsigned long long)compiled.stats.entries,
        (unsigned long long)compiled.stats.returns, (unsigned long long)compiled.stats.deopts);
    if (compiled.stats.functions_compiled == 0 || compiled.stats.entries < 3 || interpreter.stats.entries != 0) {
      fprintf(stderr, "expected the loop to tier up and be entered, and the JIT-off run not to\n");
      goto done;
    }
  }
  else {
    printf("this build has no JIT (JIT=no): both runs were the interpreter's, and every counter is zero\n");
    if (compiled.stats.entries != 0 || compiled.stats.functions_compiled != 0) {
      fprintf(stderr, "the counters of a build without the JIT must be zero\n");
      goto done;
    }
  }
  status = 0;

done:
  gltang_program_release(program);
  gltang_tree_destroy(tree);
  return status;
}
