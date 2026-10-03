/*
 * SPDX-License-Identifier: LGPL-3.0-only
 * Copyright (C) 2026 Corey Pennycuff
 */

/**
 * @file
 *
 * libFuzzer harness: parse, compile and run, under a small budget.
 *
 * The first byte picks script or template mode; the rest is the source. A
 * program that parses and compiles is run with a few thousand units of fuel,
 * a small memory budget and a shallow call budget, so a loop or a runaway
 * recursion ends as a pause or an unwind. Neither is a failure: the contract
 * is that any bytes give an answer from the interface, never a crash, a hang
 * or a leak. The group is built on the default allocator, so a leak is
 * ASan's to find.
 *
 * Build with: make fuzz-run     Run: make fuzz-run-run
 * Replay the corpus without libFuzzer: make fuzz-replay
 */

// First, before any system header: it sets a feature test macro.
#include "lastInput.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/lang-tang/compile.h>
#include <ghoti.io/lang-tang/execution.h>
#include <ghoti.io/lang-tang/lang-tang.h>
#include <ghoti.io/lang-tang/program.h>
#include <ghoti.io/lang-tang/value.h>
#include <ghoti.io/runtime-core/runtime-core.h>
#include <ghoti.io/runtime-heap/runtime-heap.h>

int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  gltang_fuzz_record_last_input("build/last-input-run.bin", data, size);
  if (size == 0) {
    return 0;
  }

  GLTANG_ParseMode mode = (data[0] & 1) ? GLTANG_PARSE_TEMPLATE : GLTANG_PARSE_SCRIPT;
  char * source = malloc(size);
  if (!source) {
    return 0;
  }
  memcpy(source, data + 1, size - 1);
  source[size - 1] = '\0';

  GLTANG_ParseError error;
  GLTANG_Tree * tree = NULL;
  GLTANG_Program * program = NULL;
  if (gltang_parse(source, mode, &error, &tree) == GLTANG_OK) {
    if (gltang_compile(tree, "fuzz", &error, &program) != GLTANG_OK) {
      program = NULL;
    }
    gltang_tree_destroy(tree);
  }
  free(source);
  if (!program) {
    return 0;
  }

  GRCORE_Group * group = NULL;
  GRCORE_Options * options = NULL;
  GRHEAP_Options * heap_options = NULL;
  GRCORE_Context * context = NULL;
  GRHEAP_Heap * heap = NULL;
  GLTANG_Execution * execution = NULL;
  if (grcore_group_create(NULL, NULL, &group) == GRCORE_OK
      && grcore_options_create(NULL, &options) == GRCORE_OK
      && grheap_options_create(NULL, &heap_options) == GRHEAP_OK) {
    grcore_options_set_fuel(options, 20000);
    grcore_options_set_memory_bytes(options, 32u << 20);
    grcore_options_set_guest_depth(options, 64);
    gltang_heap_options_configure(heap_options);
    if (grcore_context_create(group, options, &context) == GRCORE_OK
        && grheap_heap_create(context, heap_options, &heap) == GRHEAP_OK
        && gltang_execution_create(context, program, &execution) == GLTANG_OK) {
      GRCORE_Outcome outcome;
      GRCORE_Result ran = grcore_run(context, gltang_execution_entry, execution, &outcome);
      if (ran == GRCORE_OK && outcome == GRCORE_OUTCOME_PAUSED) {
        // A pause is an answer; so is more fuel and one more try.
        grcore_context_set_fuel(context, grcore_context_fuel_used(context) + 20000);
        (void)grcore_resume(context, &outcome);
      }
      // Whatever the run came to, the result and the output are readable.
      size_t length = 0;
      (void)gltang_execution_result_kind(execution);
      (void)gltang_execution_output_raw(execution, &length);
      char * rendered = NULL;
      if (gltang_execution_output_render(execution, &rendered, &length) == GLTANG_OK) {
        gltang_buffer_free(rendered);
      }
      char * described = NULL;
      if (gltang_execution_result_describe(execution, &described, &length) == GLTANG_OK) {
        gltang_buffer_free(described);
      }
    }
  }
  if (context) {
    grcore_context_destroy(context);
  }
  grheap_options_destroy(heap_options);
  grcore_options_destroy(options);
  if (group) {
    grcore_group_destroy(group);
  }
  gltang_program_release(program);
  return 0;
}
