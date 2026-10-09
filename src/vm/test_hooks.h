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
#include <ghoti.io/lang-tang/execution.h>
#include <ghoti.io/lang-tang/library.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Turns off compiled calls, fails rebuilds, or makes every push a GC point. */
void gltang_vm_set_jit_test_switches_unchecked(GLTANG_Execution * execution, bool calls_off, bool fail_rebuild, bool gc_at_push);

/** Turns off compiled native calls and loads, or makes the native-record seam a GC point. */
void gltang_vm_set_native_switches_unchecked(GLTANG_Execution * execution, bool natives_off, bool gc_seam);

/** How many natives the execution has entered through the shared wrapper, from either tier. */
uint64_t gltang_vm_test_natives_called(const GLTANG_Execution * execution);

/** Adds the test native of the given kind (see testnatives.c) to a library. */
GLTANG_Result gltang_vm_test_add_native(GLTANG_Library * library, const char * name, int kind);

#ifdef __cplusplus
}
#endif

#endif
