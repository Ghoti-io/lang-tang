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
 * The internal hooks the tests, the fuzzers and the benchmark use. They are
 * not part of the installed interface: this header is not installed, and every
 * user (the definitions in `src/` and the callers outside it) includes it, so
 * that a signature that drifts is a compile error and not a silent link.
 */

#ifndef GHOTI_IO_GLTANG_VM_TEST_HOOKS_H
#define GHOTI_IO_GLTANG_VM_TEST_HOOKS_H

#include <ghoti.io/lang-tang/macros.h>
#include <ghoti.io/lang-tang/description.h>
#include <ghoti.io/lang-tang/execution.h>
#include <ghoti.io/lang-tang/library.h>
#include <ghoti.io/runtime-core/a/deopt.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Sets a description's limits as they are, with no ceiling and whatever it holds,
 * so that a test can build one the engine's own limits refuse (past the ceilings,
 * or deeper than the engine nests values) and show that the materializer refuses
 * it again, or lower them after the building to show the same.
 */
void gltang_vm_test_description_limits(GLTANG_ValueDescription * description, size_t max_nodes, size_t max_bytes, size_t max_depth);

/**
 * Lowers the ceilings (nodes and bytes) the materializer holds every description
 * to, whatever its own limits say, so that a test reaches them with a few nodes;
 * pass GLTANG_VALUE_MAX_NODES and GLTANG_VALUE_MAX_BYTES to put them back.
 */
void gltang_vm_test_materialize_ceilings(size_t max_nodes, size_t max_bytes);

/** Turns off compiled calls, fails rebuilds, or makes every push a GC point. */
void gltang_vm_set_jit_test_switches_unchecked(GLTANG_Execution * execution, bool calls_off, bool fail_rebuild, bool gc_at_push);

/** Turns off compiled native calls and loads, or makes the native-record seam a GC point. */
void gltang_vm_set_native_switches_unchecked(GLTANG_Execution * execution, bool natives_off, bool gc_seam);

/** How many natives the execution has entered through the shared wrapper, from either tier. */
uint64_t gltang_vm_test_natives_called(const GLTANG_Execution * execution);

/**
 * Whether a frame word of kind VALUE is a float, and if so its 64 bits (sign and
 * payload of a NaN included). The inspect text rounds to six decimals and prints
 * every NaN as nan, so the frame differential reads a float slot through this.
 */
bool gltang_vm_test_float_bits(uint64_t value, uint64_t * bits);

/**
 * The text the engine's inspector gives a float whose 64 bits are `bits`, as
 * `snprintf` returns it (the length the full text needs). A compiled frame holds
 * a float raw until it is rebuilt, and the frame differential must read it as the
 * value it stands for without boxing it (that would change what it observes), so it
 * renders the bits through this. `context` must belong to a live execution.
 */
size_t gltang_vm_test_float_text(const GRCORE_Context * context, uint64_t bits, char * buffer, size_t size);

/** Refuses the Nth nonzero extension of the reservation, or the pool's top-up after it (0: never). */
void gltang_vm_set_float_test_switches_unchecked(GLTANG_Execution * execution, uint64_t fail_extend_at, uint64_t fail_pool_at);

/** The pool's boxes, the reservation's capacity and the extensions open now. Returns 1 if there is a JIT. */
int gltang_vm_jit_test_boxes(GLTANG_Execution * execution, uint64_t * pool, uint64_t * capacity, uint64_t * extensions);

/** Refuses the stack push of the Nth push hook that extended the reservation, and what the reservation held after the refusal was undone. */
void gltang_vm_set_stack_push_failure_unchecked(GLTANG_Execution * execution, uint64_t fail_at);

/** Refuses the Nth entry slot the JIT makes (0: never), as an allocation failure would: the compile that needs it fails and is counted. */
void gltang_vm_set_slot_failure_unchecked(GLTANG_Execution * execution, uint64_t fail_at);
/** Discards a compiled function now (see the eighth counted exit); 1 if it was compiled. */
int gltang_vm_jit_test_discard(GLTANG_Execution * execution, uint64_t fword);

/** Counted deoptimizations charged to this exact program-qualified function; -1 if it has no JIT record. */
int gltang_vm_jit_test_function_deopts(GLTANG_Execution * execution, uint64_t fword);

/** Marks a function never-compile (its record's raw-float count goes to zero). */
int gltang_vm_jit_test_mark_never(GLTANG_Execution * execution, uint64_t fword);
uint64_t gltang_vm_jit_test_stray_words(GLTANG_Execution * execution);
void gltang_vm_jit_test_failed_push(GLTANG_Execution * execution, uint64_t * capacity, uint64_t * extensions, uint64_t * count);

/** The reservation the JIT extends, the engine's id, and a way to extend (level 0) and give back everything. */
GRCORE_DeoptReservation * gltang_vm_jit_test_reservation(GLTANG_Execution * execution);
GRCORE_EngineId gltang_vm_test_engine_id(const GLTANG_Execution * execution);
bool gltang_vm_jit_test_extend(GLTANG_Execution * execution, size_t cells);
void gltang_vm_jit_test_retract_all(GLTANG_Execution * execution);

/** What the JIT compiler's kind tables say about an opcode (bit 0: reads a raw operand, bit 1: known effect on kinds). */
unsigned gltang_vm_jit_test_opcode_policy(unsigned op);

/** Adds the test native of the given kind (see testnatives.c) to a library. */
GLTANG_Result gltang_vm_test_add_native(GLTANG_Library * library, const char * name, int kind);

#ifdef __cplusplus
}
#endif

#endif
