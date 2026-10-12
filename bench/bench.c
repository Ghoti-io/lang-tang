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
 * drawn in a loop), and of the baseline JIT (a 10-million-iteration integer loop
 * interpreted and compiled, the same loop with four statements an iteration so
 * that the cost of a poll can be read from the difference, and a small function
 * run once interpreted and once with a threshold of one, the difference being
 * the compile; the cases exist only when the library has the JIT), and of the opt-in statement poll (a loop of four statements
 * an iteration with statement polls off, and on with nothing pending), and of snapshots (the time from creating a context to its being paused right after a heavy prologue, started from scratch and started by restoring a snapshot taken there; CAP-11). An engine case's unit is one run of a fixed program; the
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

#include "../src/vm/test_hooks.h"
#include <ghoti.io/lang-tang/compile.h>
#include <ghoti.io/lang-tang/execution.h>
#include <ghoti.io/lang-tang/lang-tang.h>
#include <ghoti.io/lang-tang/library.h>
#include <ghoti.io/lang-tang/program.h>
#include <ghoti.io/lang-tang/value.h>
#include <ghoti.io/runtime-core/runtime-core.h>
#include <ghoti.io/runtime-heap/runtime-heap.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* A row of a macro-generated table that some build leaves out. */
#define BENCH_UNUSED __attribute__((unused))

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

/* The context of the execution a Setup is being called for, for a case whose
 * setup needs the context (the profiler attaches to it). */
static GRCORE_Context * setup_context;

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
  /* Calls between compiled functions are compiled only under a native-stack
   * budget (AD-28); a case that measures them needs one. 1 MiB is the `tang`
   * command's default. */
  grcore_options_set_native_stack_bytes(options, (uint64_t)1 << 20);
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
#ifdef GLTANG_WITH_JIT
/* The JIT's counters at the end of the last run of the last case that asked
 * (a call-heavy case prints them: the figure is only the compiled calls' if
 * calls were made, and no call between compiled functions left compiled code). */
static GLTANG_JitStats last_jit_stats;
#endif
/* When set, the integer a run must give: a figure for a program that computed
 * something else is a figure of something else. */
static int have_expected_integer;
static int64_t expected_integer;
/* The same for a float result, compared by its bits (a case that computes a float is
 * checked against a C reference built with the project's -ffp-contract=off, so the
 * bits are the interpreter's too); expected_float_nan accepts any NaN. */
static int have_expected_float;
static uint64_t expected_float_bits;
static int expected_float_nan;
/* What the last run of the last case that asked left behind: the host returns it
 * made (pauses), and the heap's counters. */
static uint64_t last_pauses;
static uint64_t last_heap_collections;
static uint64_t last_heap_allocations;

static uint64_t bits_of(double d) {
  uint64_t bits;
  memcpy(&bits, &d, sizeof bits);
  return bits;
}

/* Whether the execution's result is of the kind and value the case computes. */
static int result_ok(const GLTANG_Execution * execution, GLTANG_ValueKind expect) {
  if (gltang_execution_result_kind(execution) != expect) {
    return 0;
  }
  if (have_expected_integer && gltang_execution_result_integer(execution) != expected_integer) {
    return 0;
  }
  if (have_expected_float) {
    double d = gltang_execution_result_float(execution);
    if (expected_float_nan ? !(d != d) : bits_of(d) != expected_float_bits) {
      return 0;
    }
  }
  return 1;
}

static uint64_t run_source_with(const char * source, uint64_t iterations, double * elapsed, GLTANG_ValueKind expect, uint64_t fuel_slice, Setup setup) {
  GLTANG_Program * program = compile_source(source, GLTANG_PARSE_SCRIPT);
  uint64_t sink = 0;
  double total = 0.0;
  for (uint64_t i = 0; i < iterations; i++) {
    Engine e;
    engine_open(&e, program, fuel_slice ? fuel_slice : GRCORE_UNLIMITED);
    if (setup) {
      setup_context = e.context;
      setup(e.execution);
    }
    GRCORE_Outcome outcome;
    uint64_t pauses = 0;
    double start = now_ns();
    GRCORE_Result r = grcore_run(e.context, gltang_execution_entry, e.execution, &outcome);
    while (r == GRCORE_OK && outcome == GRCORE_OUTCOME_PAUSED) {
      ++pauses;
      grcore_context_set_fuel(e.context, grcore_context_fuel_used(e.context) + fuel_slice);
      r = grcore_resume(e.context, &outcome);
    }
    total += now_ns() - start;
    if (r != GRCORE_OK || gltang_execution_result_kind(e.execution) != expect) {
      setup_failed(source);
    }
    if (!result_ok(e.execution, expect)) {
      setup_failed("a result other than the one the case computes");
    }
    last_pauses = pauses;
    {
      GRHEAP_Stats heap_stats;
      memset(&heap_stats, 0, sizeof heap_stats);
      if (grheap_stats(e.heap, &heap_stats) == GRHEAP_OK) {
        last_heap_collections = heap_stats.collections;
        last_heap_allocations = heap_stats.allocations;
      }
    }
#ifdef GLTANG_WITH_JIT
    (void)gltang_execution_jit_stats(e.execution, &last_jit_stats);
#endif
    sink += (uint64_t)gltang_execution_result_integer(e.execution) + gltang_execution_result_size(e.execution);
    if (expect == GLTANG_KIND_FLOAT) {
      sink += bits_of(gltang_execution_result_float(e.execution));
    }
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

#ifdef GLTANG_WITH_JIT
/* The baseline JIT (story 15). Each run is one compile of a hot loop's source
 * in a context of its own, so the clock covers the run, which for the
 * compiled cases includes the compile and the entry (both are small beside ten
 * million iterations). --smoke shrinks the loop so that `make test` stays quick. */
static int smoke_mode;

static uint64_t loop_count(void) {
  return smoke_mode ? 100000u : 10000000u;
}

static void setup_jit_off(GLTANG_Execution * execution) {
  if (gltang_execution_set_jit_threshold(execution, 0) != GLTANG_OK) {
    setup_failed("the JIT threshold");
  }
}

static void setup_jit_on(GLTANG_Execution * execution) {
  if (gltang_execution_set_jit_threshold(execution, 1) != GLTANG_OK) {
    setup_failed("the JIT threshold");
  }
}

/* `body` is the loop's statements; the loop counts i up to n. A body of four
 * statements steps four times as far each iteration, so the same number of
 * statements run in a quarter of the polls. */
static uint64_t jit_loop(const char * body, uint64_t step, Setup setup, uint64_t iterations, double * elapsed) {
  char source[512];
  snprintf(source, sizeof(source), "function f(n) { i = 0; while (i < n) { %s } return i; } f(%llu);", body, (unsigned long long)(loop_count() / step * step));
  return run_source_with(source, iterations, elapsed, GLTANG_KIND_INTEGER, 0, setup);
}

static uint64_t jit_loop_interpreted_run(uint64_t iterations, double * elapsed) {
  return jit_loop("i = i + 1;", 1, setup_jit_off, iterations, elapsed);
}

static uint64_t jit_loop_compiled_run(uint64_t iterations, double * elapsed) {
  return jit_loop("i = i + 1;", 1, setup_jit_on, iterations, elapsed);
}

static uint64_t jit_loop4_interpreted_run(uint64_t iterations, double * elapsed) {
  return jit_loop("i = i + 1; i = i + 1; i = i + 1; i = i + 1;", 4, setup_jit_off, iterations, elapsed);
}

static uint64_t jit_loop4_compiled_run(uint64_t iterations, double * elapsed) {
  return jit_loop("i = i + 1; i = i + 1; i = i + 1; i = i + 1;", 4, setup_jit_on, iterations, elapsed);
}

/* The cost of sampling: the same interpreted loop with no profiler and with the
 * profiler attached and its timer at one millisecond, which is how a host would
 * run it. The timer thread only posts a request; a sample is taken at the next
 * poll. At 1 kHz the difference is a few samples' worth of frame walks per
 * second of run, and the figure says how large that is beside the loop. */
static void setup_profiled(GLTANG_Execution * execution) {
  (void)execution;
  GRCORE_Profiler * profiler;
  if (gltang_execution_set_jit_threshold(execution, 0) != GLTANG_OK
      || grcore_profiler_attach(setup_context, 0, &profiler) != GRCORE_OK
      || grcore_profiler_timer_start(profiler, 1000) != GRCORE_OK) {
    setup_failed("the profiler");
  }
}

static uint64_t profile_loop_off_run(uint64_t iterations, double * elapsed) {
  return jit_loop("i = i + 1;", 1, setup_jit_off, iterations, elapsed);
}

static uint64_t profile_loop_1ms_run(uint64_t iterations, double * elapsed) {
  return jit_loop("i = i + 1;", 1, setup_profiled, iterations, elapsed);
}

/* A typical small function, run once: nothing hot, so the difference between
 * the compiled case and the interpreted one is the time to compile it and to
 * enter the code (and nothing the function does). */
#define SMALL_FUNCTION_SOURCE \
  "function f(a, b) { c = a * 2 + b; if (c > 10) { c = c - 10; } else { c = c + 10; } i = 0; while (i < 3) { c = c + i; i = i + 1; } return c - a; } f(7, 3);"

static uint64_t small_function_interpreted_run(uint64_t iterations, double * elapsed) {
  return run_source_with(SMALL_FUNCTION_SOURCE, iterations, elapsed, GLTANG_KIND_INTEGER, 0, setup_jit_off);
}

static uint64_t small_function_compiled_run(uint64_t iterations, double * elapsed) {
  return run_source_with(SMALL_FUNCTION_SOURCE, iterations, elapsed, GLTANG_KIND_INTEGER, 0, setup_jit_on);
}

/* The call-heavy case: fib with the JIT at its default threshold is the
 * compiled side; this is the same program interpreted, and with no call site
 * compiled (milestone 1's behaviour, where a compiled function left its code at
 * every CALL and the JIT could only cost). The result is checked, and the
 * counters of the last run are printed with the figure. */
#define FIB_SOURCE "function fib(n) { if (n < 2) { return n; } return fib(n - 1) + fib(n - 2); } fib(%d);"


/* The control: the JIT at its default threshold with no call site compiled, so
 * every CALL is an exit as in milestone 1. */
static void setup_calls_off(GLTANG_Execution * execution) {
  gltang_vm_set_jit_test_switches_unchecked(execution, true, false, false);
}

static uint64_t fib_source_run(int n, int64_t value, Setup setup, uint64_t iterations, double * elapsed) {
  char source[256];
  snprintf(source, sizeof(source), FIB_SOURCE, n);
  have_expected_integer = 1;
  expected_integer = value;
  uint64_t sink = run_source_with(source, iterations, elapsed, GLTANG_KIND_INTEGER, 0, setup);
  have_expected_integer = 0;
  return sink;
}

static uint64_t fib15_interpreted_run(uint64_t iterations, double * elapsed) {
  return fib_source_run(15, 610, setup_jit_off, iterations, elapsed);
}

static uint64_t fib15_default_run(uint64_t iterations, double * elapsed) {
  return fib_source_run(15, 610, NULL, iterations, elapsed);
}

static uint64_t fib15_calls_off_run(uint64_t iterations, double * elapsed) {
  return fib_source_run(15, 610, setup_calls_off, iterations, elapsed);
}

static uint64_t fib22_interpreted_run(uint64_t iterations, double * elapsed) {
  return fib_source_run(22, 17711, setup_jit_off, iterations, elapsed);
}

static uint64_t fib22_default_run(uint64_t iterations, double * elapsed) {
  return fib_source_run(22, 17711, NULL, iterations, elapsed);
}

static uint64_t fib22_calls_off_run(uint64_t iterations, double * elapsed) {
  return fib_source_run(22, 17711, setup_calls_off, iterations, elapsed);
}

/* Library calls from compiled code (spec-runtime-calls story 9, CAP-7): the loop
 * of `native_call_run` in a function, a tenth of the loop count of the other JIT
 * cases (a call costs more than an add). Interpreted, compiled (the call of the
 * native and the load of `use` are compiled and make no exit), and compiled with
 * natives switched off (the behaviour before the story: the call is an exit at
 * every iteration). The result is checked. */

static void setup_native_interpreted(GLTANG_Execution * execution) {
  setup_native(execution);
  setup_jit_off(execution);
}

static void setup_native_compiled(GLTANG_Execution * execution) {
  setup_native(execution);
  setup_jit_on(execution);
}

static void setup_native_natives_off(GLTANG_Execution * execution) {
  setup_native(execution);
  setup_jit_on(execution);
  gltang_vm_set_native_switches_unchecked(execution, true, false);
}

static uint64_t jit_native_run(Setup setup, uint64_t iterations, double * elapsed) {
  uint64_t n = loop_count() / 10u;
  char source[256];
  snprintf(source, sizeof(source), "function f(n) { use inc; s = 0; i = 0; while (i < n) { s = inc(s); i = i + 1; } return s; } f(%llu);", (unsigned long long)n);
  have_expected_integer = 1;
  expected_integer = (int64_t)n;
  uint64_t sink = run_source_with(source, iterations, elapsed, GLTANG_KIND_INTEGER, 0, setup);
  have_expected_integer = 0;
  return sink;
}

static uint64_t jit_native_interpreted_run(uint64_t iterations, double * elapsed) {
  return jit_native_run(setup_native_interpreted, iterations, elapsed);
}

static uint64_t jit_native_compiled_run(uint64_t iterations, double * elapsed) {
  return jit_native_run(setup_native_compiled, iterations, elapsed);
}

static uint64_t jit_native_off_run(uint64_t iterations, double * elapsed) {
  return jit_native_run(setup_native_natives_off, iterations, elapsed);
}

/* Steady state (spec-runtime-calls story 10 follow-up). Every case above builds a fresh
 * engine for each timed run, so each run pays the interpreter's warm-up and, for the compiled
 * cases, the tier-up compile, and throws the compiled code away. A host that compiles a
 * template or a function once and calls it many times does not. These cases use ONE
 * execution per timed run: a script calls the function once (the first call, which
 * includes the tier-up), many more times (the warm-up), and then a counted number of
 * times, and a host native, `tick`, stamps the clock between the phases. The clock
 * therefore covers the calls only (and two native calls, about 60 ns each, of the
 * 4,000,000 ns and up that a phase takes). The public API has no way to call a guest
 * function from the host, so the script loop is the way; each call enters the function
 * from the interpreter's top level, as a host's call of it would. The first-call cases
 * report the first call, in a script that stops after it; the warm-call cases report the
 * cost of one call in the timed phase. The tier-up and compile are the difference between the two. */
#define STEADY_TICKS 16
static double steady_ticks[STEADY_TICKS];
static int steady_tick_count;

static bool bench_tick(GLTANG_NativeCall * call, void * user) {
  (void)user;
  if (steady_tick_count < STEADY_TICKS) {
    steady_ticks[steady_tick_count++] = now_ns();
  }
  gltang_call_return_integer(call, 0);
  return true;
}

static void setup_tick(GLTANG_Execution * execution, int with_template) {
  GLTANG_Library * library = NULL;
  if (gltang_library_create(NULL, &library) != GLTANG_OK
      || gltang_library_add_native(library, "tick", bench_tick, NULL) != GLTANG_OK
      || (with_template && gltang_library_add_template(library, "t", bench_template(), 100000, GLTANG_SCOPE_EMPTY) != GLTANG_OK)
      || gltang_execution_set_libraries(execution, library) != GLTANG_OK) {
    setup_failed("a tick library");
  }
  gltang_library_release(library);
}

/* The general form: @p stamps ticks are expected, and the figure of a run is the
 * interval @p which (ticks[which + 1] less ticks[which]) divided by @p divisor. The
 * result is checked against the expectation globals. @p jit_mode: 0 the JIT off,
 * 1 its default threshold, 2 a threshold of one (the function is compiled at its
 * first entry). */
static uint64_t steady_run_x(const char * source, GLTANG_ValueKind kind, int with_template, int jit_mode, int stamps, int which, double divisor, uint64_t iterations, double * elapsed) {
  GLTANG_Program * program = compile_source(source, GLTANG_PARSE_SCRIPT);
  uint64_t sink = 0;
  double total = 0.0;
  for (uint64_t i = 0; i < iterations; i++) {
    Engine e;
    engine_open(&e, program, GRCORE_UNLIMITED);
    setup_tick(e.execution, with_template);
    if (jit_mode == 0) {
      setup_jit_off(e.execution);
    }
    else if (jit_mode == 2) {
      setup_jit_on(e.execution);
    }
    steady_tick_count = 0;
    GRCORE_Outcome outcome;
    GRCORE_Result r = grcore_run(e.context, gltang_execution_entry, e.execution, &outcome);
    if (r != GRCORE_OK || steady_tick_count != stamps || !result_ok(e.execution, kind)) {
      setup_failed("a steady-state case that did not compute its answer");
    }
    total += (steady_ticks[which + 1] - steady_ticks[which]) / divisor;
    (void)gltang_execution_jit_stats(e.execution, &last_jit_stats);
    {
      GRHEAP_Stats heap_stats;
      memset(&heap_stats, 0, sizeof heap_stats);
      if (grheap_stats(e.heap, &heap_stats) == GRHEAP_OK) {
        last_heap_collections = heap_stats.collections;
        last_heap_allocations = heap_stats.allocations;
      }
    }
    last_pauses = 0;
    sink += (uint64_t)gltang_execution_result_integer(e.execution);
    if (kind == GLTANG_KIND_FLOAT) {
      sink += bits_of(gltang_execution_result_float(e.execution));
    }
    engine_close(&e);
  }
  *elapsed = total;
  gltang_program_release(program);
  return sink;
}

/* which: 0 the first call, 1 one call of the timed phase. */
static uint64_t steady_run(const char * source, int64_t expected, int with_template, int jit_on, int which, uint64_t timed, uint64_t iterations, double * elapsed) {
  have_expected_integer = 1;
  expected_integer = expected;
  uint64_t sink = steady_run_x(source, GLTANG_KIND_INTEGER, with_template, jit_on ? 1 : 0, 4, which == 0 ? 0 : 2, which == 0 ? 1.0 : (double)timed, iterations, elapsed);
  have_expected_integer = 0;
  return sink;
}

static uint64_t steady_fib(int n, int64_t value, int jit_on, int which, uint64_t warm, uint64_t timed, uint64_t iterations, double * elapsed) {
  char source[640];
  snprintf(source, sizeof(source),
      "function fib(n) { if (n < 2) { return n; } return fib(n - 1) + fib(n - 2); } use tick; tick(); a = fib(%d); tick(); w = 0; "
      "while (w < %llu) { fib(%d); w = w + 1; } tick(); s = 0; i = 0; while (i < %llu) { s = s + fib(%d); i = i + 1; } tick(); s;",
      n, (unsigned long long)warm, n, (unsigned long long)timed, n);
  return steady_run(source, value * (int64_t)timed, 0, jit_on, which, timed, iterations, elapsed);
}

static uint64_t steady_fib15_interpreted_first(uint64_t it, double * el) { return steady_fib(15, 610, 0, 0, 0, 0, it, el); }
static uint64_t steady_fib15_compiled_first(uint64_t it, double * el) { return steady_fib(15, 610, 1, 0, 0, 0, it, el); }
static uint64_t steady_fib15_interpreted_warm(uint64_t it, double * el) { return steady_fib(15, 610, 0, 1, 20, 200, it, el); }
static uint64_t steady_fib15_compiled_warm(uint64_t it, double * el) { return steady_fib(15, 610, 1, 1, 20, 200, it, el); }
static uint64_t steady_fib22_interpreted_first(uint64_t it, double * el) { return steady_fib(22, 17711, 0, 0, 0, 0, it, el); }
static uint64_t steady_fib22_compiled_first(uint64_t it, double * el) { return steady_fib(22, 17711, 1, 0, 0, 0, it, el); }
static uint64_t steady_fib22_interpreted_warm(uint64_t it, double * el) { return steady_fib(22, 17711, 0, 1, 2, 20, it, el); }
static uint64_t steady_fib22_compiled_warm(uint64_t it, double * el) { return steady_fib(22, 17711, 1, 1, 2, 20, it, el); }

/* A template called repeatedly in one execution. The template's function 0 is counted at
 * its entry poll, once for each call, so the default threshold (200) is crossed by the
 * warm-up and its compiled code, if it can be compiled, stays in the execution's table
 * for the calls after. The template's output is 13 bytes. */
static uint64_t steady_template(int jit_on, int which, uint64_t warm, uint64_t timed, uint64_t iterations, double * elapsed) {
  char source[512];
  snprintf(source, sizeof(source),
      "use t; use tick; tick(); n = t().length; tick(); i = 0; while (i < %llu) { n += t().length; i += 1; } tick(); "
      "i = 0; while (i < %llu) { n += t().length; i += 1; } tick(); n;",
      (unsigned long long)warm, (unsigned long long)timed);
  return steady_run(source, (int64_t)(13u * (1u + warm + timed)), 1, jit_on, which, timed, iterations, elapsed);
}

static uint64_t steady_template_interpreted_first(uint64_t it, double * el) { return steady_template(0, 0, 0, 0, it, el); }
static uint64_t steady_template_compiled_first(uint64_t it, double * el) { return steady_template(1, 0, 0, 0, it, el); }
static uint64_t steady_template_interpreted_warm(uint64_t it, double * el) { return steady_template(0, 1, 400, 2000, it, el); }
static uint64_t steady_template_compiled_warm(uint64_t it, double * el) { return steady_template(1, 1, 400, 2000, it, el); }

/* ---- Floats and division (spec-runtime-float story 8, CAP-6) ----------------------------
 *
 * Each program is a Tang function `f(n)` with a C reference of the same loop (built with
 * the project's -ffp-contract=off, so a float result must agree bit for bit; an integer
 * one by value). The answer is checked inside the case, in --smoke too: a figure for a
 * program that computed something else is a figure of something else.
 *
 * Two methodologies, never merged. "jit-*" cases are a fresh engine for each timed run
 * (the run pays the interpreter's first touches, the counting to the threshold, the
 * compile and the entry, and throws the code away; the compiled side runs at a threshold
 * of one). "steady-*" cases use one execution for each timed run: a script calls the
 * function once (the first call), W more times (the warm-up) and T times (the counted
 * phase), and the host native `tick` stamps the clock between them; the compiled side runs
 * at a threshold of one, so the first call includes the compile.
 *
 * A function that returns a float exits compiled code once at the return (compiled code
 * cannot allocate, AD-27), and the eighth exit discards the compiled function; so the
 * warm calls of a `return s;` program are the interpreter's after the discard. That is
 * the finding of the "ret" variants, and the "cast" variants (`return s as int;`, no
 * exit) are the steady state of the same loop that stays compiled. */

typedef struct {
  const char * source; /* function f(n) { ... } */
  int is_float;
  int64_t (*ref_int)(uint64_t n);
  double (*ref_float)(uint64_t n);
} FloatProgram;

static double ref_loop(uint64_t n) {
  double s = 0.0;
  for (uint64_t i = 0; i < n; i++) {
    s = s + (double)(int64_t)i * 0.5;
  }
  return s;
}
static int64_t ref_loop_cast(uint64_t n) { return (int64_t)ref_loop(n); }
static double ref_mixed_div(uint64_t n) {
  double s = 0.0;
  for (uint64_t i = 0; i < n; i++) {
    s = s + (double)(int64_t)i / 3.0;
  }
  return s;
}
static double ref_mixed_mul(uint64_t n) {
  double s = 0.0;
  for (uint64_t i = 0; i < n; i++) {
    s = s * 0.999 + (double)(int64_t)i;
  }
  return s;
}
static int64_t ref_cmp(uint64_t n) {
  int64_t c = 0;
  const double y = 100.0;
  for (uint64_t i = 0; i < n; i++) {
    double x = (double)(int64_t)i * 0.5;
    c += x < y;
    c += x == y;
    c += x >= y;
  }
  return c;
}
static int64_t ref_div3(uint64_t n) {
  int64_t s = 0;
  for (int64_t i = 0; i < (int64_t)n; i++) {
    s += i / 3;
  }
  return s;
}
static int64_t ref_mod7(uint64_t n) {
  int64_t s = 0;
  for (int64_t i = 0; i < (int64_t)n; i++) {
    s += i % 7;
  }
  return s;
}
static int64_t ref_div_var(uint64_t n) {
  int64_t s = 0;
  for (int64_t i = 0; i < (int64_t)n; i++) {
    s += i / ((i % 5) + 1);
  }
  return s;
}
static int64_t ref_mod_var(uint64_t n) {
  int64_t s = 0;
  for (int64_t i = 0; i < (int64_t)n; i++) {
    s += i % ((i % 5) + 1);
  }
  return s;
}
static int64_t ref_cast_int(uint64_t n) {
  int64_t s = 0;
  for (uint64_t i = 0; i < n; i++) {
    s += (int64_t)((double)(int64_t)i * 0.5);
  }
  return s;
}
static double ref_cast_float(uint64_t n) {
  double x = 0.0;
  for (uint64_t i = 0; i < n; i++) {
    x = x + (double)(int64_t)i;
  }
  return x;
}

#define LOOP_HEAD(init) "function f(n) { " init " i = 0; while (i < n) { "
#define LOOP_TAIL " i = i + 1; } return s; }"

static const FloatProgram programs[] = {
  /* 0: case 1, the accumulator from 0.0, returned (one exit, at the return) */
  {LOOP_HEAD("s = 0.0;") "s = s + i * 0.5;" LOOP_TAIL, 1, NULL, ref_loop},
  /* 1: the same loop returned through `as int` (no exit: the steady state that stays compiled) */
  {"function f(n) { s = 0.0; i = 0; while (i < n) { s = s + i * 0.5; i = i + 1; } return s as int; }", 0, ref_loop_cast, NULL},
  /* 2: case 2, the accumulator from the integer 0 (the merge rule: no raw slot) */
  {LOOP_HEAD("s = 0;") "s = s + i * 0.5;" LOOP_TAIL, 1, NULL, ref_loop},
  /* 3, 4: case 3, mixed integer and float */
  {LOOP_HEAD("s = 0.0;") "s = s + (i as float) / 3.0;" LOOP_TAIL, 1, NULL, ref_mixed_div},
  {LOOP_HEAD("s = 0.0;") "s = s * 0.999 + i;" LOOP_TAIL, 1, NULL, ref_mixed_mul},
  /* 5: case 4, float comparisons over raw operands */
  {"function f(n) { x = 0.0; y = 100.0; c = 0; i = 0; while (i < n) { x = i * 0.5; if (x < y) { c = c + 1; } "
   "if (x == y) { c = c + 1; } if (x >= y) { c = c + 1; } i = i + 1; } return c; }", 0, ref_cmp, NULL},
  /* 6 to 9: case 5, integer division and modulo (constant and steered divisors; no guard fails) */
  {LOOP_HEAD("s = 0;") "s = s + i / 3;" LOOP_TAIL, 0, ref_div3, NULL},
  {LOOP_HEAD("s = 0;") "s = s + i % 7;" LOOP_TAIL, 0, ref_mod7, NULL},
  {LOOP_HEAD("s = 0;") "s = s + i / ((i % 5) + 1);" LOOP_TAIL, 0, ref_div_var, NULL},
  {LOOP_HEAD("s = 0;") "s = s + i % ((i % 5) + 1);" LOOP_TAIL, 0, ref_mod_var, NULL},
  /* 10, 11: case 6, the casts */
  {LOOP_HEAD("s = 0;") "s = s + (i * 0.5) as int;" LOOP_TAIL, 0, ref_cast_int, NULL},
  {"function f(n) { s = 0.0; i = 0; while (i < n) { s = s + (i as float); i = i + 1; } return s; }", 1, NULL, ref_cast_float},
};

static uint64_t float_n(void) { return smoke_mode ? 20000u : 1000000u; }
static uint64_t float_steady_n(void) { return smoke_mode ? 1000u : 100000u; }
static uint64_t float_warm(void) { return smoke_mode ? 2u : 10u; }
static uint64_t float_timed(void) { return smoke_mode ? 3u : 40u; }

/* Sets the expectation globals for program @p p at @p n; clear_expectations undoes it. */
static void set_expectations(const FloatProgram * p, uint64_t n) {
  if (p->is_float) {
    have_expected_float = 1;
    expected_float_bits = bits_of(p->ref_float(n));
    expected_float_nan = 0;
  }
  else {
    have_expected_integer = 1;
    expected_integer = p->ref_int(n);
  }
}

static void clear_expectations(void) {
  have_expected_integer = 0;
  have_expected_float = 0;
  expected_float_nan = 0;
}

static uint64_t program_fresh(int idx, int jit_on, uint64_t iterations, double * elapsed) {
  const FloatProgram * p = &programs[idx];
  uint64_t n = float_n();
  char source[1024];
  snprintf(source, sizeof(source), "%s f(%llu);", p->source, (unsigned long long)n);
  set_expectations(p, n);
  uint64_t sink = run_source_with(source, iterations, elapsed, p->is_float ? GLTANG_KIND_FLOAT : GLTANG_KIND_INTEGER, 0,
      jit_on ? setup_jit_on : setup_jit_off);
  clear_expectations();
  return sink;
}

/* which: 0 the first call; 1 one call of the counted phase (after the warm-up). */
static uint64_t program_steady(int idx, int jit_on, int which, uint64_t iterations, double * elapsed) {
  const FloatProgram * p = &programs[idx];
  uint64_t n = float_steady_n();
  char source[1280];
  snprintf(source, sizeof(source),
      "%s use tick; tick(); r = f(%llu); tick(); w = 0; while (w < %llu) { f(%llu); w = w + 1; } tick(); "
      "i = 0; while (i < %llu) { r = f(%llu); i = i + 1; } tick(); r;",
      p->source, (unsigned long long)n, (unsigned long long)(which == 0 ? 0u : float_warm()), (unsigned long long)n,
      (unsigned long long)(which == 0 ? 0u : float_timed()), (unsigned long long)n);
  set_expectations(p, n);
  uint64_t sink = steady_run_x(source, p->is_float ? GLTANG_KIND_FLOAT : GLTANG_KIND_INTEGER, 0, jit_on ? 2 : 0, 4,
      which == 0 ? 0 : 2, which == 0 ? 1.0 : (double)float_timed(), iterations, elapsed);
  clear_expectations();
  return sink;
}

#define PROGRAM_FUNCTIONS(tag, idx) \
  static uint64_t tag##_fresh_i(uint64_t it, double * el) { return program_fresh(idx, 0, it, el); } \
  static uint64_t tag##_fresh_c(uint64_t it, double * el) { return program_fresh(idx, 1, it, el); } \
  static uint64_t tag##_first_i(uint64_t it, double * el) { return program_steady(idx, 0, 0, it, el); } \
  static uint64_t tag##_first_c(uint64_t it, double * el) { return program_steady(idx, 1, 0, it, el); } \
  static uint64_t tag##_warm_i(uint64_t it, double * el) { return program_steady(idx, 0, 1, it, el); } \
  static uint64_t tag##_warm_c(uint64_t it, double * el) { return program_steady(idx, 1, 1, it, el); }

PROGRAM_FUNCTIONS(p_loop_ret, 0)
PROGRAM_FUNCTIONS(p_loop_cast, 1)
PROGRAM_FUNCTIONS(p_merge_int0, 2)
PROGRAM_FUNCTIONS(p_mixed_div, 3)
PROGRAM_FUNCTIONS(p_mixed_mul, 4)
PROGRAM_FUNCTIONS(p_cmp, 5)
PROGRAM_FUNCTIONS(p_div3, 6)
PROGRAM_FUNCTIONS(p_mod7, 7)
PROGRAM_FUNCTIONS(p_div_var, 8)
PROGRAM_FUNCTIONS(p_mod_var, 9)
PROGRAM_FUNCTIONS(p_cast_int, 10)
PROGRAM_FUNCTIONS(p_cast_float, 11)

/* The case rows of one program: the fresh-engine pair, then the steady-state first-call and
 * warm-call pairs. @p fi and @p fc are the iteration counts of the interpreted and compiled
 * fresh cases, @p si and @p sc of the steady ones. */
#define PROGRAM_ROWS(fresh, steady, tag, fi, fc, si, sc) \
  {fresh "-interpreted", tag##_fresh_i, fi, 1u}, \
  {fresh "-compiled", tag##_fresh_c, fc, 1u}, \
  {steady "-interpreted-first-call", tag##_first_i, si * 10u, 1u}, \
  {steady "-compiled-first-call", tag##_first_c, sc * 10u, 1u}, \
  {steady "-interpreted-warm-call", tag##_warm_i, si, 1u}, \
  {steady "-compiled-warm-call", tag##_warm_c, sc, 1u},

/* ---- The function that ends in a float exit, and its controls (cases 7 to 11) ---- */

typedef enum {
  FK_RET,       /* a = 1.5; loop L times; return a * 2.0;               ends in a float return (an exit) */
  FK_PRINT,     /* ...; print(a * 2.0); return 0;                       ends in a print of a raw float (an exit) */
  FK_CAST,      /* ...; return (a * 2.0) as int;                        the same float work, no float exit */
  FK_INT,       /* a = 1; loop L times; return a * 2;                   integer only: no pool */
  FK_GLOBAL,    /* ...; gv = a * 2.0; return 0;                          a store of a raw float to a global (an exit) */
  FK_ARG,       /* ...; h(a * 2.0); return 0;                            a raw float as a call argument (an exit) */
  FK_E_DIV0,    /* return x / 0.0;                                      an error at every call */
  FK_E_NANINT,  /* return x as int; x = NaN                             an error at every call */
  FK_E_MOD,     /* return x % 2.0;                                      an error at every call */
  FK_E_STR,     /* return x * "a";                                      an error at every call */
  FK_N_INFSUB,  /* y = x * 1.0; z = y - y; z != z ? 1 : 0; x = inf      NaN arithmetic over raw operands: not an exit */
  FK_N_ZERODIV  /* z = 0.0; (z / x) as int; x = 2.0                     a zero result: not an exit */
} FamilyKind;

static void family_function(FamilyKind k, unsigned loop, char * out, size_t size) {
  switch (k) {
    case FK_RET:
      snprintf(out, size, "function g() { a = 1.5; i = 0; while (i < %u) { a = a + 1.0; i = i + 1; } return a * 2.0; }", loop);
      return;
    case FK_PRINT:
      snprintf(out, size, "function g() { a = 1.5; i = 0; while (i < %u) { a = a + 1.0; i = i + 1; } print(a * 2.0); return 0; }", loop);
      return;
    case FK_CAST:
      snprintf(out, size, "function g() { a = 1.5; i = 0; while (i < %u) { a = a + 1.0; i = i + 1; } return (a * 2.0) as int; }", loop);
      return;
    case FK_INT:
      snprintf(out, size, "function g() { a = 1; i = 0; while (i < %u) { a = a + 1; i = i + 1; } return a * 2; }", loop);
      return;
    case FK_GLOBAL:
      snprintf(out, size, "function g() { global gv; a = 1.5; i = 0; while (i < %u) { a = a + 1.0; i = i + 1; } gv = a * 2.0; return 0; }", loop);
      return;
    case FK_ARG:
      snprintf(out, size, "function h(x) { return 0; } function g() { a = 1.5; i = 0; while (i < %u) { a = a + 1.0; i = i + 1; } h(a * 2.0); return 0; }", loop);
      return;
    case FK_E_DIV0: snprintf(out, size, "function g(x) { return x / 0.0; }"); return;
    case FK_E_NANINT: snprintf(out, size, "function g(x) { return x as int; }"); return;
    case FK_E_MOD: snprintf(out, size, "function g(x) { return x %% 2.0; }"); return;
    case FK_E_STR: snprintf(out, size, "function g(x) { return x * \"a\"; }"); return;
    case FK_N_INFSUB: snprintf(out, size, "function g(x) { y = x * 1.0; z = y - y; return z != z ? 1 : 0; }"); return;
    case FK_N_ZERODIV: snprintf(out, size, "function g(x) { z = 0.0; return (z / x) as int; }"); return;
  }
}

static int family_has_argument(FamilyKind k) { return k >= FK_E_DIV0; }
static int family_ends_in_error(FamilyKind k) { return k >= FK_E_DIV0 && k <= FK_E_STR; }

/* The statements that make the argument `av` before the first tick. 1000.0 times ten four
 * hundred times is an infinity. */
static const char * family_prelude(FamilyKind k) {
  switch (k) {
    case FK_E_NANINT: return "y = 1000.0; k = 0; while (k < 400) { y = y * 10.0; k = k + 1; } av = y - y;";
    case FK_N_INFSUB: return "y = 1000.0; k = 0; while (k < 400) { y = y * 10.0; k = k + 1; } av = y;";
    case FK_N_ZERODIV: return "av = 2.0;";
    case FK_GLOBAL: return "gv = 0;";
    default: return "av = 1.5;";
  }
}

/* The expectation of a family program's last result. Returns the kind. */
static GLTANG_ValueKind family_expectation(FamilyKind k, unsigned loop) {
  clear_expectations();
  double a = 1.5;
  switch (k) {
    case FK_RET:
      for (unsigned i = 0; i < loop; i++) {
        a = a + 1.0;
      }
      have_expected_float = 1;
      expected_float_bits = bits_of(a * 2.0);
      return GLTANG_KIND_FLOAT;
    case FK_CAST:
      for (unsigned i = 0; i < loop; i++) {
        a = a + 1.0;
      }
      have_expected_integer = 1;
      expected_integer = (int64_t)(a * 2.0);
      return GLTANG_KIND_INTEGER;
    case FK_INT:
      have_expected_integer = 1;
      expected_integer = 2 + 2 * (int64_t)loop;
      return GLTANG_KIND_INTEGER;
    case FK_N_INFSUB:
      have_expected_integer = 1;
      expected_integer = 1;
      return GLTANG_KIND_INTEGER;
    default: /* PRINT returns 0, the zero divisor gives 0, an error case's script ends in 0 */
      have_expected_integer = 1;
      expected_integer = 0;
      return GLTANG_KIND_INTEGER;
  }
}

/* The script of a steady family case: call 1 (stamp 0 to 1), calls 2 to 8 (1 to 2), call 9
 * (2 to 3) and the calls after it, K of them (3 to 4). With a threshold of one the first
 * eight calls are compiled; a function that exits at every call is discarded at the eighth
 * exit, and the calls after run in the interpreter. */
static void family_script(FamilyKind k, unsigned loop, uint64_t middle, uint64_t ninth, uint64_t after, char * out, size_t size) {
  char function[256];
  family_function(k, loop, function, sizeof function);
  const char * arg = family_has_argument(k) ? "av" : "";
  const char * last = family_ends_in_error(k) ? "0;" : "r;";
  /* The loops after call 1 are cut short for the interval being measured (the stamps stay: five). */
  snprintf(out, size,
      "%s %s use tick; tick(); r = g(%s); tick(); i = 0; while (i < %llu) { r = g(%s); i = i + 1; } tick(); "
      "i = 0; while (i < %llu) { r = g(%s); i = i + 1; } tick(); "
      "i = 0; while (i < %llu) { r = g(%s); i = i + 1; } tick(); %s",
      function, family_prelude(k), arg, (unsigned long long)middle, arg, (unsigned long long)ninth, arg,
      (unsigned long long)after, arg, last);
}

static uint64_t family_after(void) { return smoke_mode ? 3u : 100u; }

/* which: 0 call 1, 1 calls 2 to 8 (per call), 2 call 9, 3 the calls after (per call). */
static uint64_t family_steady(FamilyKind k, unsigned loop, int jit_on, int which, uint64_t iterations, double * elapsed) {
  char source[1024];
  family_script(k, loop, which >= 1 ? 7u : 0u, which >= 2 ? 1u : 0u, which >= 3 ? family_after() : 0u, source, sizeof source);
  GLTANG_ValueKind kind = family_expectation(k, loop);
  static const double divisor[4] = {1.0, 7.0, 1.0, 0.0};
  uint64_t sink = steady_run_x(source, kind, 0, jit_on ? 2 : 0, 5, which, which == 3 ? (double)family_after() : divisor[which], iterations, elapsed);
  clear_expectations();
  return sink;
}

/* A fresh engine, and the run is one call (the compile, the entry and the exit are in it). */
static uint64_t family_fresh(FamilyKind k, unsigned loop, int jit_on, uint64_t iterations, double * elapsed) {
  char function[256];
  char source[512];
  family_function(k, loop, function, sizeof function);
  snprintf(source, sizeof source, "%s g();", function);
  GLTANG_ValueKind kind = family_expectation(k, loop);
  uint64_t sink = run_source_with(source, iterations, elapsed, kind, 0, jit_on ? setup_jit_on : setup_jit_off);
  clear_expectations();
  return sink;
}

#define FAMILY_FUNCTIONS(tag, kind, loop) \
  BENCH_UNUSED static uint64_t tag##_fresh_i(uint64_t it, double * el) { return family_fresh(kind, loop, 0, it, el); } \
  BENCH_UNUSED static uint64_t tag##_fresh_c(uint64_t it, double * el) { return family_fresh(kind, loop, 1, it, el); } \
  static uint64_t tag##_s0_i(uint64_t it, double * el) { return family_steady(kind, loop, 0, 0, it, el); } \
  static uint64_t tag##_s1_i(uint64_t it, double * el) { return family_steady(kind, loop, 0, 1, it, el); } \
  static uint64_t tag##_s2_i(uint64_t it, double * el) { return family_steady(kind, loop, 0, 2, it, el); } \
  static uint64_t tag##_s3_i(uint64_t it, double * el) { return family_steady(kind, loop, 0, 3, it, el); } \
  static uint64_t tag##_s0_c(uint64_t it, double * el) { return family_steady(kind, loop, 1, 0, it, el); } \
  static uint64_t tag##_s1_c(uint64_t it, double * el) { return family_steady(kind, loop, 1, 1, it, el); } \
  static uint64_t tag##_s2_c(uint64_t it, double * el) { return family_steady(kind, loop, 1, 2, it, el); } \
  static uint64_t tag##_s3_c(uint64_t it, double * el) { return family_steady(kind, loop, 1, 3, it, el); }

FAMILY_FUNCTIONS(f_ret0, FK_RET, 0u)
FAMILY_FUNCTIONS(f_ret100, FK_RET, 100u)
FAMILY_FUNCTIONS(f_ret10k, FK_RET, 10000u)
FAMILY_FUNCTIONS(f_print0, FK_PRINT, 0u)
FAMILY_FUNCTIONS(f_print100, FK_PRINT, 100u)
FAMILY_FUNCTIONS(f_print10k, FK_PRINT, 10000u)
FAMILY_FUNCTIONS(f_cast0, FK_CAST, 0u)
FAMILY_FUNCTIONS(f_cast100, FK_CAST, 100u)
FAMILY_FUNCTIONS(f_cast10k, FK_CAST, 10000u)
FAMILY_FUNCTIONS(f_int0, FK_INT, 0u)
FAMILY_FUNCTIONS(f_int100, FK_INT, 100u)
FAMILY_FUNCTIONS(f_int10k, FK_INT, 10000u)
FAMILY_FUNCTIONS(f_global0, FK_GLOBAL, 0u)
FAMILY_FUNCTIONS(f_global100, FK_GLOBAL, 100u)
FAMILY_FUNCTIONS(f_arg0, FK_ARG, 0u)
FAMILY_FUNCTIONS(f_arg100, FK_ARG, 100u)
FAMILY_FUNCTIONS(f_div0, FK_E_DIV0, 0u)
FAMILY_FUNCTIONS(f_nanint, FK_E_NANINT, 0u)
FAMILY_FUNCTIONS(f_mod, FK_E_MOD, 0u)
FAMILY_FUNCTIONS(f_str, FK_E_STR, 0u)
FAMILY_FUNCTIONS(f_infsub, FK_N_INFSUB, 0u)
FAMILY_FUNCTIONS(f_zerodiv, FK_N_ZERODIV, 0u)

/* All the rows of one family program: the fresh pair and the four steady intervals, interpreted
 * and compiled. */
#define FAMILY_ROWS(name, tag, it) \
  {name "-interpreted", tag##_fresh_i, it, 1u}, \
  {name "-compiled", tag##_fresh_c, it, 1u}, \
  {"steady-" name "-interpreted-call1", tag##_s0_i, it, 1u}, \
  {"steady-" name "-interpreted-calls2to8", tag##_s1_i, it, 1u}, \
  {"steady-" name "-interpreted-call9", tag##_s2_i, it, 1u}, \
  {"steady-" name "-interpreted-calls10on", tag##_s3_i, it, 1u}, \
  {"steady-" name "-compiled-call1", tag##_s0_c, it, 1u}, \
  {"steady-" name "-compiled-calls2to8", tag##_s1_c, it, 1u}, \
  {"steady-" name "-compiled-call9", tag##_s2_c, it, 1u}, \
  {"steady-" name "-compiled-calls10on", tag##_s3_c, it, 1u},

/* The rows of a program that has only the steady forms (an error or a NaN control). */
#define STEADY_FAMILY_ROWS(name, tag, it) \
  {"steady-" name "-interpreted-call1", tag##_s0_i, it, 1u}, \
  {"steady-" name "-interpreted-calls2to8", tag##_s1_i, it, 1u}, \
  {"steady-" name "-interpreted-call9", tag##_s2_i, it, 1u}, \
  {"steady-" name "-interpreted-calls10on", tag##_s3_i, it, 1u}, \
  {"steady-" name "-compiled-call1", tag##_s0_c, it, 1u}, \
  {"steady-" name "-compiled-calls2to8", tag##_s1_c, it, 1u}, \
  {"steady-" name "-compiled-call9", tag##_s2_c, it, 1u}, \
  {"steady-" name "-compiled-calls10on", tag##_s3_c, it, 1u},

/* ---- The pool dropped at every host return (case 10) ----
 *
 * `g()` of the cast family (float work, no float exit) is called in a loop with the run
 * sliced by fuel: each pause returns control to the host, which lets the float boxes go
 * (gltang_jit_release_pool), so the next entry into compiled code makes them again. The
 * unit of the figure is one host return (the elapsed time is divided by the pauses a run
 * took), and the counters give boxes made per host return. The check of the answer is the
 * sum of the calls' results. */
static uint64_t request_pattern(uint64_t slice, int jit_on, uint64_t iterations, double * elapsed) {
  uint64_t calls = smoke_mode ? 200u : 4000u;
  char function[256];
  char source[512];
  family_function(FK_CAST, 0u, function, sizeof function);
  snprintf(source, sizeof source, "%s s = 0; i = 0; while (i < %llu) { s = s + g(); i = i + 1; } s;", function, (unsigned long long)calls);
  have_expected_integer = 1;
  expected_integer = (int64_t)(3u * calls);
  uint64_t sink = run_source_with(source, iterations, elapsed, GLTANG_KIND_INTEGER, slice, jit_on ? setup_jit_on : setup_jit_off);
  have_expected_integer = 0;
  if (last_pauses == 0) {
    setup_failed("a sliced run that never paused");
  }
  *elapsed /= (double)last_pauses;
  return sink;
}

static uint64_t request_s30_i(uint64_t it, double * el) { return request_pattern(30, 0, it, el); }
static uint64_t request_s30_c(uint64_t it, double * el) { return request_pattern(30, 1, it, el); }
static uint64_t request_s300_i(uint64_t it, double * el) { return request_pattern(300, 0, it, el); }
static uint64_t request_s300_c(uint64_t it, double * el) { return request_pattern(300, 1, it, el); }
static uint64_t request_s3000_i(uint64_t it, double * el) { return request_pattern(3000, 0, it, el); }
static uint64_t request_s3000_c(uint64_t it, double * el) { return request_pattern(3000, 1, it, el); }
#endif

/* ---- Snapshots: a start from scratch against a start from a snapshot ---- */

/* A program with a heavy prologue: a table of 20,000 integers and a map of
 * 2,000 entries, built by loops, and then a long loop that reads the table.
 * The host's ready state is "the prologue has run": the context is paused right
 * after it, which is what a snapshot is for. A fuel budget that lands just
 * after the prologue is the cost of a copy of the program that stops after it. */
static const char snapshot_prologue[] =
  "table = [];\n"
  "index = {:};\n"
  "for (i = 0; i < 20000; i += 1) { table[i] = i * 7; }\n"
  "for (j = 0; j < 2000; j += 1) { index[\"k\" + (j as string)] = [j, j * j]; }\n"
  "total = 0;\n";
static const char snapshot_tail[] = "for (k = 0; k < 1000000; k += 1) { total += table[k % 20000]; }\ntotal;";
static const char snapshot_short_tail[] = "for (k = 0; k < 1; k += 1) { total += table[k]; }\ntotal;";

static uint64_t prologue_fuel(void) {
  char source[1024];
  snprintf(source, sizeof(source), "%s%s", snapshot_prologue, snapshot_short_tail);
  GLTANG_Program * program = compile_source(source, GLTANG_PARSE_SCRIPT);
  Engine e;
  engine_open(&e, program, GRCORE_UNLIMITED);
  GRCORE_Outcome outcome;
  if (grcore_run(e.context, gltang_execution_entry, e.execution, &outcome) != GRCORE_OK || outcome != GRCORE_OUTCOME_FINISHED) {
    setup_failed("the prologue calibration");
  }
  uint64_t fuel = grcore_context_fuel_used(e.context);
  engine_close(&e);
  gltang_program_release(program);
  return fuel;
}

/* One start from scratch: a context, a heap and an execution are made, and the
 * program is run until it pauses right after the prologue. An iteration is one
 * start, from creating the context to being paused at the ready point. */
static uint64_t start_scratch_run(uint64_t iterations, double * elapsed) {
  char source[1024];
  snprintf(source, sizeof(source), "%s%s", snapshot_prologue, snapshot_tail);
  GLTANG_Program * program = compile_source(source, GLTANG_PARSE_SCRIPT);
  uint64_t fuel = prologue_fuel();
  uint64_t sink = 0;
  double total = 0.0;
  for (uint64_t i = 0; i < iterations; i++) {
    double start = now_ns();
    Engine e;
    engine_open(&e, program, fuel);
    GRCORE_Outcome outcome;
    if (grcore_run(e.context, gltang_execution_entry, e.execution, &outcome) != GRCORE_OK || outcome != GRCORE_OUTCOME_PAUSED) {
      setup_failed("the start from scratch");
    }
    total += now_ns() - start;
    sink += grcore_context_fuel_used(e.context);
    engine_close(&e);
  }
  *elapsed = total;
  gltang_program_release(program);
  return sink;
}

/* One start from a snapshot taken at that ready point (once, outside the
 * clock): a context, a heap and an execution are made and the snapshot restored
 * into them. The same iteration, from creating the context to being paused at
 * the ready point. */
static uint64_t start_snapshot_run(uint64_t iterations, double * elapsed) {
  char source[1024];
  snprintf(source, sizeof(source), "%s%s", snapshot_prologue, snapshot_tail);
  GLTANG_Program * program = compile_source(source, GLTANG_PARSE_SCRIPT);
  uint64_t fuel = prologue_fuel();
  Engine origin;
  engine_open(&origin, program, fuel);
  GRCORE_Outcome outcome;
  if (grcore_run(origin.context, gltang_execution_entry, origin.execution, &outcome) != GRCORE_OK || outcome != GRCORE_OUTCOME_PAUSED) {
    setup_failed("the origin of the snapshot");
  }
  GLTANG_Snapshot * snapshot = NULL;
  if (gltang_snapshot_take(origin.execution, &snapshot) != GLTANG_OK) {
    setup_failed("the snapshot");
  }
  uint64_t sink = gltang_snapshot_size(snapshot);
  double total = 0.0;
  for (uint64_t i = 0; i < iterations; i++) {
    double start = now_ns();
    Engine e;
    engine_open(&e, program, GRCORE_UNLIMITED);
    if (gltang_snapshot_restore(e.execution, snapshot) != GLTANG_OK) {
      setup_failed("the start from the snapshot");
    }
    total += now_ns() - start;
    if (grcore_context_state(e.context) != GRCORE_CONTEXT_PAUSED) {
      setup_failed("the restored context is not paused");
    }
    sink += (uint64_t)i;
    engine_close(&e);
  }
  *elapsed = total;
  gltang_snapshot_release(snapshot);
  engine_close(&origin);
  gltang_program_release(program);
  return sink;
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
    {"start-scratch", start_scratch_run, 100u, 2u},
    {"start-snapshot", start_snapshot_run, 2000u, 2u},
#ifdef GLTANG_WITH_JIT
    {"jit-loop-10M-interpreted", jit_loop_interpreted_run, 3u, 1u},
    {"jit-loop-10M-compiled", jit_loop_compiled_run, 10u, 1u},
    {"jit-loop4-10M-interpreted", jit_loop4_interpreted_run, 3u, 1u},
    {"jit-loop4-10M-compiled", jit_loop4_compiled_run, 10u, 1u},
    {"profile-loop-10M-off", profile_loop_off_run, 3u, 1u},
    {"profile-loop-10M-1ms", profile_loop_1ms_run, 3u, 1u},
    {"jit-small-function-interpreted", small_function_interpreted_run, 5000u, 5u},
    {"jit-small-function-compiled", small_function_compiled_run, 5000u, 5u},
    {"fib-15-interpreted", fib15_interpreted_run, 1000u, 2u},
    {"fib-15-jit-default", fib15_default_run, 1000u, 2u},
    {"fib-15-jit-calls-off", fib15_calls_off_run, 1000u, 2u},
    {"fib-22-interpreted", fib22_interpreted_run, 100u, 1u},
    {"fib-22-jit-default", fib22_default_run, 100u, 1u},
    {"fib-22-jit-calls-off", fib22_calls_off_run, 100u, 1u},
    {"jit-native-call-1M-interpreted", jit_native_interpreted_run, 5u, 1u},
    {"jit-native-call-1M-compiled", jit_native_compiled_run, 10u, 1u},
    {"jit-native-call-1M-natives-off", jit_native_off_run, 5u, 1u},
    {"steady-fib-15-interpreted-first-call", steady_fib15_interpreted_first, 1000u, 2u},
    {"steady-fib-15-compiled-first-call", steady_fib15_compiled_first, 1000u, 2u},
    {"steady-fib-15-interpreted-warm-call", steady_fib15_interpreted_warm, 20u, 2u},
    {"steady-fib-15-compiled-warm-call", steady_fib15_compiled_warm, 20u, 2u},
    {"steady-fib-22-interpreted-first-call", steady_fib22_interpreted_first, 100u, 1u},
    {"steady-fib-22-compiled-first-call", steady_fib22_compiled_first, 100u, 1u},
    {"steady-fib-22-interpreted-warm-call", steady_fib22_interpreted_warm, 5u, 1u},
    {"steady-fib-22-compiled-warm-call", steady_fib22_compiled_warm, 5u, 1u},
    {"steady-template-interpreted-first-call", steady_template_interpreted_first, 1000u, 2u},
    {"steady-template-compiled-first-call", steady_template_compiled_first, 1000u, 2u},
    {"steady-template-interpreted-warm-call", steady_template_interpreted_warm, 50u, 2u},
    {"steady-template-compiled-warm-call", steady_template_compiled_warm, 50u, 2u},
    PROGRAM_ROWS("jit-float-loop-ret", "steady-float-loop-ret", p_loop_ret, 3u, 20u, 2u, 8u)
    PROGRAM_ROWS("jit-float-loop-cast", "steady-float-loop-cast", p_loop_cast, 3u, 20u, 2u, 8u)
    PROGRAM_ROWS("jit-float-merge-int0", "steady-float-merge-int0", p_merge_int0, 3u, 3u, 2u, 2u)
    PROGRAM_ROWS("jit-float-mixed-div", "steady-float-mixed-div", p_mixed_div, 3u, 20u, 2u, 8u)
    PROGRAM_ROWS("jit-float-mixed-mul", "steady-float-mixed-mul", p_mixed_mul, 3u, 20u, 2u, 8u)
    PROGRAM_ROWS("jit-float-cmp", "steady-float-cmp", p_cmp, 3u, 20u, 2u, 8u)
    PROGRAM_ROWS("jit-intdiv-div3", "steady-float-intdiv-div3", p_div3, 3u, 20u, 2u, 8u)
    PROGRAM_ROWS("jit-intdiv-mod7", "steady-float-intdiv-mod7", p_mod7, 3u, 20u, 2u, 8u)
    PROGRAM_ROWS("jit-intdiv-div-var", "steady-float-intdiv-div-var", p_div_var, 3u, 20u, 2u, 8u)
    PROGRAM_ROWS("jit-intdiv-mod-var", "steady-float-intdiv-mod-var", p_mod_var, 3u, 20u, 2u, 8u)
    PROGRAM_ROWS("jit-float-cast-int", "steady-float-cast-int", p_cast_int, 3u, 20u, 2u, 8u)
    PROGRAM_ROWS("jit-float-cast-float", "steady-float-cast-float", p_cast_float, 3u, 20u, 2u, 8u)
    FAMILY_ROWS("float-return-L0", f_ret0, 500u)
    FAMILY_ROWS("float-return-L100", f_ret100, 200u)
    FAMILY_ROWS("float-return-L10000", f_ret10k, 8u)
    FAMILY_ROWS("float-print-L0", f_print0, 500u)
    FAMILY_ROWS("float-print-L100", f_print100, 200u)
    FAMILY_ROWS("float-print-L10000", f_print10k, 8u)
    FAMILY_ROWS("float-entry-cast-L0", f_cast0, 500u)
    FAMILY_ROWS("float-entry-cast-L100", f_cast100, 200u)
    FAMILY_ROWS("float-entry-cast-L10000", f_cast10k, 8u)
    FAMILY_ROWS("float-entry-int-L0", f_int0, 500u)
    FAMILY_ROWS("float-entry-int-L100", f_int100, 200u)
    FAMILY_ROWS("float-entry-int-L10000", f_int10k, 8u)
    FAMILY_ROWS("float-store-L0", f_global0, 500u)
    FAMILY_ROWS("float-store-L100", f_global100, 200u)
    FAMILY_ROWS("float-arg-L0", f_arg0, 500u)
    FAMILY_ROWS("float-arg-L100", f_arg100, 200u)
    STEADY_FAMILY_ROWS("float-exit-div0", f_div0, 500u)
    STEADY_FAMILY_ROWS("float-exit-nanint", f_nanint, 500u)
    STEADY_FAMILY_ROWS("float-exit-mod", f_mod, 500u)
    STEADY_FAMILY_ROWS("float-exit-str", f_str, 500u)
    STEADY_FAMILY_ROWS("float-exit-ctl-infsub", f_infsub, 500u)
    STEADY_FAMILY_ROWS("float-exit-ctl-zerodiv", f_zerodiv, 500u)
    {"float-request-slice30-interpreted", request_s30_i, 5u, 1u},
    {"float-request-slice30-compiled", request_s30_c, 5u, 1u},
    {"float-request-slice300-interpreted", request_s300_i, 5u, 1u},
    {"float-request-slice300-compiled", request_s300_c, 5u, 1u},
    {"float-request-slice3000-interpreted", request_s3000_i, 5u, 1u},
    {"float-request-slice3000-compiled", request_s3000_c, 5u, 1u},
#endif
};

#define REPEATS 7

static int compare_double(const void * a, const void * b) {
  double x = *(const double *)a;
  double y = *(const double *)b;
  return (x > y) - (x < y);
}

int main(int argc, char ** argv) {
  int smoke = 0;
  const char * only = NULL; /* run the cases whose name begins with this */
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--only") == 0 && i + 1 < argc) {
      only = argv[++i];
    }
    else if (strcmp(argv[i], "--smoke") == 0) {
      smoke = 1;
#ifdef GLTANG_WITH_JIT
      smoke_mode = 1;
#endif
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
    if (only && strncmp(cases[c].name, only, strlen(only)) != 0) {
      continue;
    }
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
#ifdef GLTANG_WITH_JIT
    if (strncmp(cases[c].name, "steady-", 7) == 0) {
      printf("  %s: compiled calls %llu, call exits %llu, functions compiled %llu, compile failures %llu, entries %llu, deopts %llu, deepest chain %llu\n",
          cases[c].name, (unsigned long long)last_jit_stats.calls,
          (unsigned long long)(last_jit_stats.call_exits_remembered + last_jit_stats.call_exits_push_refused + last_jit_stats.call_exits_callee_guard + last_jit_stats.call_exits_native_stack),
          (unsigned long long)last_jit_stats.functions_compiled, (unsigned long long)last_jit_stats.compile_failures,
          (unsigned long long)last_jit_stats.entries, (unsigned long long)last_jit_stats.deopts, (unsigned long long)last_jit_stats.deepest_chain);
    }
    if (strncmp(cases[c].name, "jit-float-", 10) == 0 || strncmp(cases[c].name, "jit-intdiv-", 11) == 0
        || strncmp(cases[c].name, "steady-float-", 13) == 0 || strncmp(cases[c].name, "float-", 6) == 0) {
      printf("  %s: entries %llu, deopts %llu, values_boxed %llu, box_pool_made %llu, functions_compiled %llu, functions_discarded %llu, compile_failures %llu, "
             "host_returns %llu, heap collections %llu, heap allocations %llu\n",
          cases[c].name, (unsigned long long)last_jit_stats.entries, (unsigned long long)last_jit_stats.deopts,
          (unsigned long long)last_jit_stats.values_boxed, (unsigned long long)last_jit_stats.box_pool_made,
          (unsigned long long)last_jit_stats.functions_compiled, (unsigned long long)last_jit_stats.functions_discarded,
          (unsigned long long)last_jit_stats.compile_failures, (unsigned long long)last_pauses,
          (unsigned long long)last_heap_collections, (unsigned long long)last_heap_allocations);
    }
    if (strncmp(cases[c].name, "fib-", 4) == 0) {
      printf("  %s: compiled calls %llu, call exits %llu (remembered %llu, push refused %llu, callee guard %llu, native stack %llu), compiled at call %llu, deepest chain %llu, hook argument errors %llu\n",
          cases[c].name, (unsigned long long)last_jit_stats.calls,
          (unsigned long long)(last_jit_stats.call_exits_remembered + last_jit_stats.call_exits_push_refused + last_jit_stats.call_exits_callee_guard + last_jit_stats.call_exits_native_stack),
          (unsigned long long)last_jit_stats.call_exits_remembered, (unsigned long long)last_jit_stats.call_exits_push_refused,
          (unsigned long long)last_jit_stats.call_exits_callee_guard, (unsigned long long)last_jit_stats.call_exits_native_stack,
          (unsigned long long)last_jit_stats.compile_at_call, (unsigned long long)last_jit_stats.deepest_chain,
          (unsigned long long)last_jit_stats.hook_argument_errors);
    }
#endif
  }
#ifndef GLTANG_WITH_JIT
  /* Named, not left out: the cases of the compiled float and division operations need the JIT. */
  printf("jit-float-*, jit-intdiv-*, steady-float-*, float-return-*, float-print-*, float-entry-*, float-exit-*, float-request-*, "
         "steady-fib-*, fib-*, jit-loop-*: skipped, this build has no JIT (JIT=no)\n");
#endif
  return 0;
}
