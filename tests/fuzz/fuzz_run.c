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
#include <ghoti.io/lang-tang/library.h>
#include <ghoti.io/lang-tang/seeds.h>
#include <ghoti.io/lang-tang/program.h>
#include <ghoti.io/lang-tang/value.h>
#include <ghoti.io/runtime-core/runtime-core.h>
#include <ghoti.io/runtime-heap/runtime-heap.h>

/* A host function: answers with its argument count plus its first argument, if
 * that is an integer, and fails when it has none. */
static bool fuzz_native(GLTANG_NativeCall * call, void * user) {
  (void)user;
  size_t count = gltang_call_count(call);
  if (!count) {
    return false;
  }
  gltang_call_return_integer(call, (int64_t)count + gltang_call_integer(call, 0));
  return true;
}

/* The templates a fuzzed program can call: one that finishes, one that never
 * does (stopped by its own tiny scope under each policy). */
static const char * const fuzz_templates[][2] = {
  {"quick", "print(\"q\"); print(!\"<\");"},
  {"runaway", "print(\"before\"); while (true) {}"},
  {"nested", "use quick; print(quick()); print(1 / 0);"},
};

static GLTANG_Program * fuzz_compile(const char * source) {
  GLTANG_Tree * tree = NULL;
  GLTANG_Program * program = NULL;
  if (gltang_parse(source, GLTANG_PARSE_SCRIPT, NULL, &tree) == GLTANG_OK) {
    if (gltang_compile(tree, "fuzz-template", NULL, &program) != GLTANG_OK) {
      program = NULL;
    }
    gltang_tree_destroy(tree);
  }
  return program;
}

/* The host API the fuzzed program reaches: a native, three templates with tiny
 * scope budgets (so scopes open, close and run out), a factory, a sub-library
 * and the built-ins. Returns the library, or NULL. */
static GLTANG_Library * fuzz_library(GLTANG_Program ** programs) {
  GLTANG_Library * root = NULL;
  if (gltang_library_create(NULL, &root) != GLTANG_OK) {
    return NULL;
  }
  (void)gltang_library_add_native(root, "native", fuzz_native, NULL);
  (void)gltang_library_add_integer(root, "n", 7);
  (void)gltang_library_add_string(root, "s", "<s>", 3, GLTANG_UNICODE_STRING_TYPE_HTML);
  static const GLTANG_ScopePolicy policies[3] = {GLTANG_SCOPE_EMPTY, GLTANG_SCOPE_SEGMENTS, GLTANG_SCOPE_EMPTY};
  static const uint64_t budgets[3] = {400, 300, 600};
  for (size_t i = 0; i < 3; ++i) {
    programs[i] = fuzz_compile(fuzz_templates[i][1]);
    if (programs[i]) {
      (void)gltang_library_add_template(root, fuzz_templates[i][0], programs[i], budgets[i], policies[i]);
    }
  }
  GLTANG_Library * user = NULL;
  if (gltang_library_create("user", &user) == GLTANG_OK) {
    (void)gltang_library_add_integer(user, "id", 42);
    (void)gltang_library_add_library(root, user);
    gltang_library_release(user);
  }
  return root;
}

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
  GLTANG_Program * template_programs[3] = {NULL, NULL, NULL};
  GLTANG_Library * library = NULL;
  GLTANG_SeedSequence * seeds = NULL;
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
      // The host API: libraries, templates, a seed sequence, and the error
      // switches (the second and third bits of the first byte).
      library = fuzz_library(template_programs);
      if (library) {
        (void)gltang_execution_set_libraries(execution, library);
      }
      if (gltang_seeds_create(1, &seeds) == GLTANG_OK) {
        (void)gltang_execution_set_seeds(execution, seeds);
      }
      (void)gltang_execution_set_name(execution, "fuzz");
      (void)gltang_execution_set_log_all_errors(execution, (data[0] & 2) != 0);
      (void)gltang_execution_set_halt_on_error(execution, (data[0] & 4) != 0);
      (void)gltang_execution_set_error_limit(execution, 64);
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
      // The error list reads back, chain and all.
      for (size_t e = 0; e < gltang_execution_error_count(execution); ++e) {
        GLTANG_ErrorEntry entry;
        (void)gltang_execution_error(execution, e, &entry);
        for (size_t k = 0; k < gltang_execution_error_chain_count(execution, e); ++k) {
          GLTANG_ErrorLink link;
          (void)gltang_execution_error_chain(execution, e, k, &link);
        }
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
  gltang_seeds_destroy(seeds);
  gltang_library_release(library);
  for (size_t i = 0; i < 3; ++i) {
    gltang_program_release(template_programs[i]);
  }
  gltang_program_release(program);
  return 0;
}
