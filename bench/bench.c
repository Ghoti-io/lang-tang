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
 * The benchmark harness (AD-26).
 *
 * Every library ships one from its first commit, so that "performant" is a
 * claim with a way to check it. Besides the calibration case it holds the
 * cases of the front end (parsing a small script, parsing a 1 MiB generated
 * template, destroying the tree that template made, compiling both) and of
 * the engine (a tight integer loop, a recursive function, string building,
 * array building, and a loop that is paused and resumed on a small fuel
 * budget) and of the host API (a `use` with a member access, a native function
 * called in a loop, a template call that opens and closes a budget scope, a
 * loop of swallowed errors with the error list at its cap, and a generator
 * drawn in a loop), and of the opt-in statement poll (a loop of four statements
 * an iteration with statement polls off, and on with nothing pending). An engine case's unit is one run of a fixed program; the
 * clock covers the run and not the building or the tearing down of the context.
 *
 * The calibration case is a fixed amount of integer work that touches no
 * library code, run the same way every real case will be, so a figure from a
 * real case can be read against the machine it was taken on. A budget recorded
 * without the calibration beside it cannot be compared across hosts or
 * compilers.
 *
 * Usage:
 *   bench           run every case: several repeats, report min and median
 *   bench --smoke   run every case once with a tiny workload (what `make
 *                   test` does); proves the harness builds, links and runs
 *
 * No numeric budget is asserted here; the first measurements are recorded in
 * documentation/design.md.
 */

/* clock_gettime(CLOCK_MONOTONIC) is POSIX, and -std=c17 hides it. */
#define _POSIX_C_SOURCE 200809L

#include <ghoti.io/lang-tang/compile.h>
#include <ghoti.io/lang-tang/execution.h>
#include <ghoti.io/lang-tang/lang-tang.h>
#include <ghoti.io/lang-tang/library.h>
#include <ghoti.io/lang-tang/program.h>
#include <ghoti.io/lang-tang/value.h>
#include <ghoti.io/runtime-core/runtime-core.h>
#include <ghoti.io/runtime-heap/runtime-heap.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
  const char * name;
  /** Performs @p iterations units of work; returns a value derived from all
   *  of it so the compiler cannot discard the loop, and stores in @p elapsed
   *  the nanoseconds the measured part took (setup is not in it). */
  uint64_t (*run)(uint64_t iterations, double * elapsed);
  uint64_t iterations;       /* per repeat, full run */
  uint64_t smoke_iterations; /* per repeat, --smoke */
} Case;

/* A case that cannot set itself up must not report a short, fast run as a
 * measurement. */
static _Noreturn void setup_failed(const char * what) {
  fprintf(stderr, "bench: setup failed: %s\n", what);
  abort();
}

static double now_ns(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0.0;
  }
  return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

/* xorshift64: one dependent chain of shifts and xors per step. The chain is
 * serial on purpose, so the figure is latency-bound and does not move with
 * how wide the host's execution units are. */
static uint64_t calibration_run(uint64_t iterations, double * elapsed) {
  uint64_t x = 0x9E3779B97F4A7C15ull;
  double start = now_ns();
  for (uint64_t i = 0; i < iterations; i++) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
  }
  *elapsed = now_ns() - start;
  return x;
}

static const char small_script[] =
  "use math.floor as floor;\n"
  "z = 3;\n"
  "function f(x) {\n"
  "  function inner(floor) {\n"
  "    global z;\n"
  "    return floor < z ? floor : z;\n"
  "  }\n"
  "  return inner(x);\n"
  "}\n"
  "for (i = 0; i < 10; i = i + 1) {\n"
  "  print(f(i) * 2 + [1, 2, 3][1]);\n"
  "}\n";

/* One unit of work is one parse and the release of its tree. */
static uint64_t parse_small_script_run(uint64_t iterations, double * elapsed) {
  uint64_t sink = 0;
  double start = now_ns();
  for (uint64_t i = 0; i < iterations; i++) {
    GLTANG_Tree * tree = NULL;
    if (gltang_parse(small_script, GLTANG_PARSE_SCRIPT, NULL, &tree) != GLTANG_OK) {
      setup_failed("parse of the small script");
    }
    sink += gltang_tree_node_count(tree);
    gltang_tree_destroy(tree);
  }
  *elapsed = now_ns() - start;
  return sink;
}

/* A template of about 1 MiB: repeated text and tags, so the scanner's text
 * mode, its code mode and the print rules all see load. */
static char * make_big_template(size_t * length_out) {
  static const char piece[] = "<p>Row <%= row %> of <%= rows %>: <% if (row % 2 == 0) { %>even<% } else { %>odd<% } %></p>\n";
  size_t piece_length = sizeof(piece) - 1;
  size_t count = (1024u * 1024u) / piece_length + 1;
  char * text = malloc(count * piece_length + 1);
  if (!text) {
    setup_failed("memory for the template");
  }
  for (size_t i = 0; i < count; i++) {
    memcpy(text + i * piece_length, piece, piece_length);
  }
  text[count * piece_length] = '\0';
  *length_out = count * piece_length;
  return text;
}

/* The time to parse (and release) one such template; an iteration is one. */
static uint64_t parse_big_template_run(uint64_t iterations, double * elapsed) {
  size_t length;
  char * text = make_big_template(&length);
  uint64_t sink = 0;
  double start = now_ns();
  for (uint64_t i = 0; i < iterations; i++) {
    GLTANG_Tree * tree = NULL;
    if (gltang_parse(text, GLTANG_PARSE_TEMPLATE, NULL, &tree) != GLTANG_OK) {
      setup_failed("parse of the big template");
    }
    sink += gltang_tree_node_count(tree);
    gltang_tree_destroy(tree);
  }
  *elapsed = now_ns() - start;
  free(text);
  return sink;
}

/* The time to release that template's tree alone: the trees are parsed
 * outside the measured region, the clock covers only the destroys. */
static uint64_t destroy_big_tree_run(uint64_t iterations, double * elapsed) {
  size_t length;
  char * text = make_big_template(&length);
  GLTANG_Tree * trees[8];
  uint64_t sink = 0;
  double total = 0.0;
  uint64_t done = 0;
  while (done < iterations) {
    uint64_t batch = iterations - done < 8 ? iterations - done : 8;
    for (uint64_t k = 0; k < batch; k++) {
      if (gltang_parse(text, GLTANG_PARSE_TEMPLATE, NULL, &trees[k]) != GLTANG_OK) {
        setup_failed("parse of the big template");
      }
      sink += gltang_tree_node_count(trees[k]);
    }
    double start = now_ns();
    for (uint64_t k = 0; k < batch; k++) {
      gltang_tree_destroy(trees[k]);
    }
    total += now_ns() - start;
    done += batch;
  }
  *elapsed = total;
  free(text);
  return sink;
}


/* Compiles a source; a case that cannot do this has nothing to measure. */
static GLTANG_Program * compile_source(const char * source, GLTANG_ParseMode mode) {
  GLTANG_Tree * tree = NULL;
  GLTANG_Program * program = NULL;
  if (gltang_parse(source, mode, NULL, &tree) != GLTANG_OK) {
    setup_failed("parse of an engine case");
  }
  if (gltang_compile(tree, "bench", NULL, &program) != GLTANG_OK) {
    setup_failed("compile of an engine case");
  }
  gltang_tree_destroy(tree);
  return program;
}

static uint64_t compile_small_script_run(uint64_t iterations, double * elapsed) {
  GLTANG_Tree * tree = NULL;
  if (gltang_parse(small_script, GLTANG_PARSE_SCRIPT, NULL, &tree) != GLTANG_OK) {
    setup_failed("parse of the small script");
  }
  uint64_t sink = 0;
  double start = now_ns();
  for (uint64_t i = 0; i < iterations; i++) {
    GLTANG_Program * program = NULL;
    if (gltang_compile(tree, "bench", NULL, &program) != GLTANG_OK) {
      setup_failed("compile of the small script");
    }
    sink += gltang_program_function_count(program) + gltang_program_function_size(program, 0);
    gltang_program_release(program);
  }
  *elapsed = now_ns() - start;
  gltang_tree_destroy(tree);
  return sink;
}

static uint64_t compile_big_template_run(uint64_t iterations, double * elapsed) {
  size_t length;
  char * text = make_big_template(&length);
  GLTANG_Tree * tree = NULL;
  if (gltang_parse(text, GLTANG_PARSE_TEMPLATE, NULL, &tree) != GLTANG_OK) {
    setup_failed("parse of the big template");
  }
  uint64_t sink = 0;
  double start = now_ns();
  for (uint64_t i = 0; i < iterations; i++) {
    GLTANG_Program * program = NULL;
    if (gltang_compile(tree, "bench", NULL, &program) != GLTANG_OK) {
      setup_failed("compile of the big template");
    }
    sink += gltang_program_function_size(program, 0);
    gltang_program_release(program);
  }
  *elapsed = now_ns() - start;
  gltang_tree_destroy(tree);
  free(text);
  return sink;
}

/* One engine: a group, a context with the given fuel, a heap, an execution. */
typedef struct {
  GRCORE_Group * group;
  GRCORE_Context * context;
  GRHEAP_Heap * heap;
  GLTANG_Execution * execution;
} Engine;

/* Fills the execution's library before the run: a case's own host API. */
typedef void (*Setup)(GLTANG_Execution * execution);

static void engine_open(Engine * e, GLTANG_Program * program, uint64_t fuel) {
  GRCORE_Options * options = NULL;
  GRHEAP_Options * heap_options = NULL;
  if (grcore_group_create(NULL, NULL, &e->group) != GRCORE_OK
      || grcore_options_create(NULL, &options) != GRCORE_OK
      || grheap_options_create(NULL, &heap_options) != GRHEAP_OK) {
    setup_failed("a group");
  }
  grcore_options_set_fuel(options, fuel);
  grcore_options_set_guest_depth(options, 1024);
  gltang_heap_options_configure(heap_options);
  if (grcore_context_create(e->group, options, &e->context) != GRCORE_OK
      || grheap_heap_create(e->context, heap_options, &e->heap) != GRHEAP_OK
      || gltang_execution_create(e->context, program, &e->execution) != GLTANG_OK) {
    setup_failed("a context");
  }
  grheap_options_destroy(heap_options);
  grcore_options_destroy(options);
}

static void engine_close(Engine * e) {
  grcore_context_destroy(e->context);
  grcore_group_destroy(e->group);
}

/* Runs the source @p iterations times; the clock covers only the runs. A
 * result of the wrong kind means the program did not do what the case says it
 * does, so the figure would be of something else. */
static uint64_t run_source_with(const char * source, uint64_t iterations, double * elapsed, GLTANG_ValueKind expect, uint64_t fuel_slice, Setup setup) {
  GLTANG_Program * program = compile_source(source, GLTANG_PARSE_SCRIPT);
  uint64_t sink = 0;
  double total = 0.0;
  for (uint64_t i = 0; i < iterations; i++) {
    Engine e;
    engine_open(&e, program, fuel_slice ? fuel_slice : GRCORE_UNLIMITED);
    if (setup) {
      setup(e.execution);
    }
    GRCORE_Outcome outcome;
    double start = now_ns();
    GRCORE_Result r = grcore_run(e.context, gltang_execution_entry, e.execution, &outcome);
    while (r == GRCORE_OK && outcome == GRCORE_OUTCOME_PAUSED) {
      grcore_context_set_fuel(e.context, grcore_context_fuel_used(e.context) + fuel_slice);
      r = grcore_resume(e.context, &outcome);
    }
    total += now_ns() - start;
    if (r != GRCORE_OK || gltang_execution_result_kind(e.execution) != expect) {
      setup_failed(source);
    }
    sink += (uint64_t)gltang_execution_result_integer(e.execution) + gltang_execution_result_size(e.execution);
    engine_close(&e);
  }
  *elapsed = total;
  gltang_program_release(program);
  return sink;
}

static uint64_t run_source(const char * source, uint64_t iterations, double * elapsed, GLTANG_ValueKind expect, uint64_t fuel_slice) {
  return run_source_with(source, iterations, elapsed, expect, fuel_slice, NULL);
}

static uint64_t run_loop_run(uint64_t iterations, double * elapsed) {
  return run_source("s = 0; for (i = 0; i < 1000; i += 1) { s += i * 2; } s;", iterations, elapsed, GLTANG_KIND_INTEGER, 0);
}

static uint64_t run_fib_run(uint64_t iterations, double * elapsed) {
  return run_source("function fib(n) { if (n < 2) { return n; } return fib(n - 1) + fib(n - 2); } fib(15);", iterations, elapsed, GLTANG_KIND_INTEGER, 0);
}

static uint64_t run_string_run(uint64_t iterations, double * elapsed) {
  return run_source("s = \"\"; for (i = 0; i < 200; i += 1) { s = s + \"ab\"; } s.length;", iterations, elapsed, GLTANG_KIND_INTEGER, 0);
}

static uint64_t run_array_run(uint64_t iterations, double * elapsed) {
  return run_source("a = []; for (i = 0; i < 1000; i += 1) { a[i] = i; } a;", iterations, elapsed, GLTANG_KIND_ARRAY, 0);
}

static uint64_t run_polling_run(uint64_t iterations, double * elapsed) {
  /* The same loop as run-loop-1000, paused and resumed every 500 units of fuel. */
  return run_source("s = 0; for (i = 0; i < 1000; i += 1) { s += i * 2; } s;", iterations, elapsed, GLTANG_KIND_INTEGER, 500);
}

/* The host API. */

static bool bench_increment(GLTANG_NativeCall * call, void * user) {
  (void)user;
  gltang_call_return_integer(call, gltang_call_integer(call, 0) + 1);
  return true;
}

static void setup_native(GLTANG_Execution * execution) {
  GLTANG_Library * library = NULL;
  if (gltang_library_create(NULL, &library) != GLTANG_OK
      || gltang_library_add_native(library, "inc", bench_increment, NULL) != GLTANG_OK
      || gltang_execution_set_libraries(execution, library) != GLTANG_OK) {
    setup_failed("a native library");
  }
  gltang_library_release(library);
}

static GLTANG_Program * bench_template(void) {
  static GLTANG_Program * program;
  if (!program) {
    program = compile_source("print(\"<li>\"); print(\"item\"); print(\"</li>\");", GLTANG_PARSE_SCRIPT);
  }
  return program;
}

static void setup_template(GLTANG_Execution * execution) {
  GLTANG_Library * library = NULL;
  if (gltang_library_create(NULL, &library) != GLTANG_OK
      || gltang_library_add_template(library, "t", bench_template(), 100000, GLTANG_SCOPE_EMPTY) != GLTANG_OK
      || gltang_execution_set_libraries(execution, library) != GLTANG_OK) {
    setup_failed("a template library");
  }
  gltang_library_release(library);
}

/* The host asks for a poll at every statement, as a host that attaches a
 * debugger does. Nothing is pending, so every one of them is the unarmed fast
 * path of a poll. */
static void setup_statement_polls(GLTANG_Execution * execution) {
  if (gltang_execution_set_statement_polls(execution, true) != GLTANG_OK) {
    setup_failed("statement polls");
  }
}

static void setup_seeds(GLTANG_Execution * execution) {
  GLTANG_SeedSequence * seeds = NULL;
  if (gltang_seeds_create(1, &seeds) != GLTANG_OK || gltang_execution_set_seeds(execution, seeds) != GLTANG_OK) {
    setup_failed("a seed sequence");
  }
  gltang_seeds_destroy(seeds);
}

static uint64_t use_member_run(uint64_t iterations, double * elapsed) {
  return run_source_with("s = 0.0; for (i = 0; i < 200; i += 1) { use math; s += math.pi; } s;", iterations, elapsed, GLTANG_KIND_FLOAT, 0, NULL);
}

static uint64_t native_call_run(uint64_t iterations, double * elapsed) {
  return run_source_with("use inc; n = 0; for (i = 0; i < 1000; i += 1) { n = inc(n); } n;", iterations, elapsed, GLTANG_KIND_INTEGER, 0, setup_native);
}

static uint64_t template_call_run(uint64_t iterations, double * elapsed) {
  return run_source_with("use t; n = 0; for (i = 0; i < 200; i += 1) { n += t().length; } n;", iterations, elapsed, GLTANG_KIND_INTEGER, 0, setup_template);
}

static uint64_t swallowed_errors_run(uint64_t iterations, double * elapsed) {
  /* 2,000 swallowed errors against the default cap of 1,024: the list is full
   * for the last 976, which are counted and not kept. */
  return run_source_with("for (i = 0; i < 2000; i += 1) { print(1 / 0); } 0;", iterations, elapsed, GLTANG_KIND_INTEGER, 0, NULL);
}

static uint64_t random_global_run(uint64_t iterations, double * elapsed) {
  return run_source_with("use random; s = 0; for (i = 0; i < 1000; i += 1) { s += random.global.next_int % 7; } s;", iterations, elapsed, GLTANG_KIND_INTEGER, 0, setup_seeds);
}

/* Four statements to a loop iteration, so that the statement boundary is a
 * visible share of the work: the same program with statement polls off (the
 * boundary is a load and a branch) and on with nothing pending (each is a
 * poll's fast path). */
#define STATEMENTS_SOURCE "s = 0; t = 0; for (i = 0; i < 1000; i += 1) { s += i; t = s; u = t; w = u; } s;"

static uint64_t statements_off_run(uint64_t iterations, double * elapsed) {
  return run_source_with(STATEMENTS_SOURCE, iterations, elapsed, GLTANG_KIND_INTEGER, 0, NULL);
}

static uint64_t statements_on_run(uint64_t iterations, double * elapsed) {
  return run_source_with(STATEMENTS_SOURCE, iterations, elapsed, GLTANG_KIND_INTEGER, 0, setup_statement_polls);
}

static const Case cases[] = {
    {"calibration", calibration_run, 200u * 1000u * 1000u, 1000u * 1000u},
    {"parse-small-script", parse_small_script_run, 100000u, 100u},
    {"parse-1MiB-template", parse_big_template_run, 20u, 1u},
    {"destroy-1MiB-template-tree", destroy_big_tree_run, 16u, 1u},
    {"compile-small-script", compile_small_script_run, 100000u, 100u},
    {"compile-1MiB-template", compile_big_template_run, 20u, 1u},
    {"run-loop-1000", run_loop_run, 5000u, 5u},
    {"run-fib-15", run_fib_run, 1000u, 2u},
    {"run-string-build-200", run_string_run, 5000u, 5u},
    {"run-array-build-1000", run_array_run, 5000u, 5u},
    {"run-polling-loop-1000", run_polling_run, 5000u, 5u},
    {"run-use-member-200", use_member_run, 5000u, 5u},
    {"run-native-call-1000", native_call_run, 5000u, 5u},
    {"run-template-call-200", template_call_run, 2000u, 5u},
    {"run-swallowed-errors-2000-at-cap", swallowed_errors_run, 500u, 2u},
    {"run-random-global-1000", random_global_run, 5000u, 5u},
    {"run-statements-1000-polls-off", statements_off_run, 5000u, 5u},
    {"run-statements-1000-polls-on", statements_on_run, 5000u, 5u},
};

#define REPEATS 7

static int compare_double(const void * a, const void * b) {
  double x = *(const double *)a;
  double y = *(const double *)b;
  return (x > y) - (x < y);
}

int main(int argc, char ** argv) {
  int smoke = 0;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--smoke") == 0) {
      smoke = 1;
    }
    else {
      fprintf(stderr, "bench: unknown argument '%s'\n", argv[i]);
      return 2;
    }
  }

  /* Naming the library's version proves the harness linked the library it
   * claims to measure, and ties every figure to the build that produced it. */
  printf("lang-tang %s, %s run\n", gltang_version_string(), smoke ? "smoke" : "full");

  int repeats = smoke ? 1 : REPEATS;
  for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
    uint64_t n = smoke ? cases[c].smoke_iterations : cases[c].iterations;
    double ns[REPEATS];
    uint64_t sink = 0;
    for (int r = 0; r < repeats; r++) {
      sink ^= cases[c].run(n, &ns[r]);
    }
    qsort(ns, (size_t)repeats, sizeof(ns[0]), compare_double);
    double best = ns[0] / (double)n;
    double median = ns[repeats / 2] / (double)n;
    if (!(best > 0.0)) {
      fprintf(stderr, "bench: %s measured no time; the clock is unusable\n", cases[c].name);
      return 1;
    }
    printf("%-30s best %12.2f ns/iter   median %12.2f ns/iter   (n=%llu, sink=%llx)\n",
        cases[c].name, best, median, (unsigned long long)n, (unsigned long long)sink);
  }
  return 0;
}
