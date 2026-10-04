/**
 * @file
 *
 * Record the input a fuzz harness is about to run, so that a death nobody can
 * attribute still hands over its reproducer.
 *
 * libFuzzer writes a crash artifact from a callback that runs *after* the
 * sanitizer has finished reporting. When the sanitizer cannot finish - the
 * signature is
 *
 *     AddressSanitizer:DEADLYSIGNAL
 *     AddressSanitizer: nested bug in the same thread, aborting.
 *
 * meaning it hit a second fatal signal while reporting the first - that
 * callback never runs, and the input that caused it is gone. That happened
 * once here, at execution 2,038,515 of a parse run, and cost a long hunt with
 * nothing to reproduce from.
 *
 * The fix is to write the input down before running it, into a file mapped
 * MAP_SHARED. The kernel writes a shared mapping back whether or not the
 * process exits cleanly, so the record survives any death short of losing
 * power - verified against SIGKILL, which leaves the last input intact.
 *
 * It costs one memcpy per execution and no syscall, so it does not distort
 * exec/s, and it is on by default deliberately: the whole difficulty with a
 * one-in-millions crash is that you cannot switch on the instrument
 * afterwards. Set LAST_INPUT_FILE to move it, or to /dev/null to disable.
 *
 * Read one back with:
 *   python3 -c "import struct,sys; d=open(sys.argv[1],'rb').read(); \
 *     n,s=struct.unpack_from('<QI',d,0); sys.stdout.buffer.write(d[12:12+s])" \
 *     build/last-input-parse.bin > repro.tang
 *
 * SPDX-License-Identifier: LGPL-3.0-only
 * Copyright (C) 2026 Corey Pennycuff
 */

#ifndef GHOTIIO_LANG_TANG_FUZZ_LAST_INPUT_H
#define GHOTIIO_LANG_TANG_FUZZ_LAST_INPUT_H

// Include this header before any system header. The harnesses build with
// -std=c17, under which glibc hides ftruncate unless a feature test macro is
// set first, and a macro defined after the libc headers have been read is too
// late to have any effect. Getting the order wrong is a compile error naming
// ftruncate rather than anything silent, which is the failure mode to want.
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
// The record is a shared memory mapping that outlives the process, which is
// mmap, and the replay and the fuzzers that include this are Linux tools. On
// Windows (where only the replay is built) recording is off, as it is when
// LAST_INPUT_FILE is /dev/null; the replay reports a crash by dying.
static inline void gltang_fuzz_record_last_input(const char * default_path,
    const uint8_t * data, size_t size) {
  (void)default_path;
  (void)data;
  (void)size;
}
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

// Larger than libFuzzer's 4096-byte default so that raising -max_len does not
// silently start truncating the record.
#define GLTANG_FUZZ_LAST_INPUT_MAX 65536

typedef struct {
  // Execution number, so a record that was never written is distinguishable
  // from one written on the very first execution.
  uint64_t counter;
  uint32_t size;
  uint8_t bytes[GLTANG_FUZZ_LAST_INPUT_MAX];
} GLTANG_Fuzz_Last_Input;

/**
 * Record one input. Call it first thing in LLVMFuzzerTestOneInput.
 *
 * A failure to set the recorder up is deliberately not fatal: it is a
 * diagnostic aid, and a fuzzer that refuses to run because it could not open a
 * scratch file would be a worse outcome than one that runs unrecorded.
 */
static inline void gltang_fuzz_record_last_input(const char * default_path,
    const uint8_t * data, size_t size) {
  static GLTANG_Fuzz_Last_Input * record;
  static int tried;

  if (!tried) {
    tried = 1;
    const char * path = getenv("LAST_INPUT_FILE");
    if (!path) {
      path = default_path;
    }
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd >= 0) {
      if (ftruncate(fd, sizeof(GLTANG_Fuzz_Last_Input)) == 0) {
        void * mapped = mmap(NULL, sizeof(GLTANG_Fuzz_Last_Input),
            PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (mapped != MAP_FAILED) {
          record = mapped;
        }
      }
      close(fd);
    }
    if (!record) {
      fprintf(stderr, "### note: could not record inputs to %s; "
          "a crash the sanitizer cannot report will not leave one ###\n", path);
    }
  }

  if (record) {
    record->counter++;
    record->size = size > GLTANG_FUZZ_LAST_INPUT_MAX
        ? GLTANG_FUZZ_LAST_INPUT_MAX
        : (uint32_t)size;
    memcpy(record->bytes, data, record->size);
  }
}

#endif // _WIN32

#endif // GHOTIIO_LANG_TANG_FUZZ_LAST_INPUT_H
