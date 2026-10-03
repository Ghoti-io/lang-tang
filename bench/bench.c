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
 * cases of the front end: parsing a small script, parsing a 1 MiB generated
 * template, and destroying the tree that template made.
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
 * No numeric budget is asserted here. AD-26 records budgets once a first
 * measurement of a real case exists.
 */

/* clock_gettime(CLOCK_MONOTONIC) is POSIX, and -std=c17 hides it. */
#define _POSIX_C_SOURCE 200809L

#include <ghoti.io/lang-tang/lang-tang.h>

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

static const Case cases[] = {
    {"calibration", calibration_run, 200u * 1000u * 1000u, 1000u * 1000u},
    {"parse-small-script", parse_small_script_run, 100000u, 100u},
    {"parse-1MiB-template", parse_big_template_run, 20u, 1u},
    {"destroy-1MiB-template-tree", destroy_big_tree_run, 16u, 1u},
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
