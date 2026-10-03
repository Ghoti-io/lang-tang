/*
 * SPDX-License-Identifier: LGPL-3.0-only
 * Copyright (C) 2026 Corey Pennycuff
 */

/**
 * @file
 *
 * libFuzzer harness: gltang_parse() and gltang_compile() in template mode.
 *
 * Ported from ctang's fuzz_template.c. The contract is the interface's: any bytes
 * give OK, FORMAT, LIMIT or OOM, never a crash and never a leak, and the tree
 * of an accepted source is counted and released.
 *
 * Build with: make fuzz-template     Run: make fuzz-run-template
 * Replay the corpus without libFuzzer: make fuzz-replay
 */

// First, before any system header: it sets a feature test macro.
#include "lastInput.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <ghoti.io/lang-tang/compile.h>
#include <ghoti.io/lang-tang/lang-tang.h>
#include <ghoti.io/lang-tang/program.h>

int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  // Record the input before touching it. A crash the sanitizer cannot report
  // takes libFuzzer's artifact with it; see lastInput.h.
  gltang_fuzz_record_last_input("build/last-input-template.bin", data, size);

  // The API takes a NUL-terminated string, so that is the contract being
  // tested: a copy with a terminator, and an embedded NUL legitimately ends
  // the source.
  char * source = malloc(size + 1);
  if (!source) {
    return 0;
  }
  memcpy(source, data, size);
  source[size] = '\0';

  GLTANG_ParseError error;
  GLTANG_Tree * tree = NULL;
  GLTANG_Result result = gltang_parse(source, GLTANG_PARSE_TEMPLATE, &error, &tree);
  if (result == GLTANG_OK) {
    (void)gltang_tree_node_count(tree);
    // The compiler is the next consumer of an accepted tree: it answers OK,
    // FORMAT (a name the parser cannot judge), LIMIT or OOM, and never leaves
    // a program behind that it refused.
    GLTANG_Program * program = NULL;
    GLTANG_Result compiled = gltang_compile(tree, "fuzz", &error, &program);
    if (compiled == GLTANG_OK) {
      if (program == NULL || gltang_program_function_count(program) < 1) {
        abort();
      }
      gltang_program_release(program);
    }
    else if (program != NULL) {
      abort();
    }
    gltang_tree_destroy(tree);
  }
  else if (result == GLTANG_ERR_FORMAT) {
    // The refusal names a place and a reason, and the message is terminated.
    if (error.line < 1 || error.column < 1 || memchr(error.message, '\0', sizeof(error.message)) == NULL) {
      abort();
    }
  }

  free(source);
  return 0;
}
