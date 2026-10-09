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
 * The natives compiled code calls with runtime-jit's `CALL_NATIVE` (AD-28,
 * AD-17): thin C functions over the engine's own operations, so that the
 * interpreter and compiled code do the same work through the same code.
 *
 * - A call of a library native, with 0 to 15 arguments. The callee value is the
 *   first parameter and the arguments follow. The thunk charges the `CALL`'s fuel
 *   (the site's checks have passed: an exit before the call has charged
 *   nothing, and the interpreter then charges it when it makes the call) and
 *   enters `gltang_vm_call_native`, the one place a native is entered.
 * - The load of a `use` path and the `.name` of a library: the interpreter's
 *   `gltang_vm_resolve` and `gltang_vm_op_attr`, called as the interpreter calls
 *   them, so the value is the one it makes, whatever the library holds now.
 *
 * Each returns the value and a status: zero, or `GRJIT_NATIVE_UNWIND` for the
 * `GLTANG_V_UNWIND` an operation returns when a runtime poll ordered the run to
 * stop, or what a test native asked of compiled code (`GRJIT_NATIVE_DEOPT`). A
 * non-zero status leaves compiled code through the chain deopt with the state
 * after the call. Every one of them is a GC point.
 */

#include <ghoti.io/lang-tang/macros.h>

#include "jit_internal.h"

#include <string.h>

_Static_assert(GRJIT_NATIVE_OK == GLTANG_NATIVE_STATUS_OK, "the engine's statuses are runtime-jit's");
_Static_assert(GRJIT_NATIVE_DEOPT == GLTANG_NATIVE_STATUS_DEOPT, "the engine's statuses are runtime-jit's");
_Static_assert(GRJIT_NATIVE_UNWIND == GLTANG_NATIVE_STATUS_UNWIND, "the engine's statuses are runtime-jit's");
_Static_assert(GLTANG_JIT_MAX_CALL_ARGS + 1u <= 16u, "a native call's callee and arguments must fit runtime-jit's native limit");

static GRJIT_NativeResult finished(const GLTANG_Execution * exec, GLTANG_Value value, bool from_native) {
  GRJIT_NativeResult result;
  result.value = value;
  result.reserved = 0;
  result.status = value == GLTANG_V_UNWIND ? GRJIT_NATIVE_UNWIND : from_native ? exec->native_status : GRJIT_NATIVE_OK;
  return result;
}

static GRJIT_NativeResult call_n(void * context, uint64_t callee, size_t argc, const uint64_t * args) {
  GLTANG_Execution * exec = gltang_vm_execution_of(context);
  // The CALL's own cost, charged now that the site's checks have passed.
  exec->pending_fuel += gltang_opcode_cost_table[GLTANG_OP_CALL];
  ++exec->jit_stats.native_calls;
  GLTANG_Value value = gltang_vm_call_native(exec, callee, argc, args, true);
  return finished(exec, value, true);
}

#define PARAMS_0
#define PARAMS_1 , uint64_t a0
#define PARAMS_2 PARAMS_1, uint64_t a1
#define PARAMS_3 PARAMS_2, uint64_t a2
#define PARAMS_4 PARAMS_3, uint64_t a3
#define PARAMS_5 PARAMS_4, uint64_t a4
#define PARAMS_6 PARAMS_5, uint64_t a5
#define PARAMS_7 PARAMS_6, uint64_t a6
#define PARAMS_8 PARAMS_7, uint64_t a7
#define PARAMS_9 PARAMS_8, uint64_t a8
#define PARAMS_10 PARAMS_9, uint64_t a9
#define PARAMS_11 PARAMS_10, uint64_t a10
#define PARAMS_12 PARAMS_11, uint64_t a11
#define PARAMS_13 PARAMS_12, uint64_t a12
#define PARAMS_14 PARAMS_13, uint64_t a13
#define PARAMS_15 PARAMS_14, uint64_t a14

#define VALUES_0 0
#define VALUES_1 a0
#define VALUES_2 VALUES_1, a1
#define VALUES_3 VALUES_2, a2
#define VALUES_4 VALUES_3, a3
#define VALUES_5 VALUES_4, a4
#define VALUES_6 VALUES_5, a5
#define VALUES_7 VALUES_6, a6
#define VALUES_8 VALUES_7, a7
#define VALUES_9 VALUES_8, a8
#define VALUES_10 VALUES_9, a9
#define VALUES_11 VALUES_10, a10
#define VALUES_12 VALUES_11, a11
#define VALUES_13 VALUES_12, a12
#define VALUES_14 VALUES_13, a13
#define VALUES_15 VALUES_14, a14

// `fn(context, callee, a0 .. a(N-1))`: the arguments gathered into an array the
// shared wrapper copies before anything can collect.
#define THUNK(N) \
  static GRJIT_NativeResult thunk_##N(void * context, uint64_t callee PARAMS_##N) { \
    const uint64_t args[N ? N : 1] = {VALUES_##N}; \
    return call_n(context, callee, N, args); \
  }

THUNK(0) THUNK(1) THUNK(2) THUNK(3) THUNK(4) THUNK(5) THUNK(6) THUNK(7)
THUNK(8) THUNK(9) THUNK(10) THUNK(11) THUNK(12) THUNK(13) THUNK(14) THUNK(15)

static GRJIT_NativeResult thunk_use(void * context, uint64_t block) {
  GLTANG_Execution * exec = gltang_vm_execution_of(context);
  exec->pending_fuel += gltang_opcode_cost_table[GLTANG_OP_USE];
  ++exec->jit_stats.member_loads;
  GLTANG_Value value = gltang_vm_resolve(exec, (const GLTANG_StringBlock *)(uintptr_t)block);
  return finished(exec, value, false);
}

static GRJIT_NativeResult thunk_attr(void * context, uint64_t container, uint64_t constant) {
  GLTANG_Execution * exec = gltang_vm_execution_of(context);
  exec->pending_fuel += gltang_opcode_cost_table[GLTANG_OP_ATTR];
  ++exec->jit_stats.member_loads;
  // As the interpreter's ATTR: the name is made (a GC point) while the library is
  // held, and the attribute is read from the library as the collector left it.
  size_t mark = gltang_vm_temp_mark(exec);
  if (!gltang_vm_temp_push(exec, container)) {
    return finished(exec, exec->roots[GLTANG_ROOT_OOM], false);
  }
  GLTANG_Value name = gltang_vm_constant(exec, (uint32_t)constant);
  container = gltang_vm_temp_at(exec, mark);
  gltang_vm_temp_release(exec, mark);
  if (name == GLTANG_V_UNWIND) {
    return finished(exec, name, false);
  }
  return finished(exec, gltang_vm_op_attr(exec, container, name), false);
}

GRJIT_Result gltang_jit_natives_create(GLTANG_Execution * exec, GRJIT_NativeTable ** out) {
  const uint64_t calls[16] = {
    (uint64_t)(uintptr_t)thunk_0, (uint64_t)(uintptr_t)thunk_1, (uint64_t)(uintptr_t)thunk_2, (uint64_t)(uintptr_t)thunk_3,
    (uint64_t)(uintptr_t)thunk_4, (uint64_t)(uintptr_t)thunk_5, (uint64_t)(uintptr_t)thunk_6, (uint64_t)(uintptr_t)thunk_7,
    (uint64_t)(uintptr_t)thunk_8, (uint64_t)(uintptr_t)thunk_9, (uint64_t)(uintptr_t)thunk_10, (uint64_t)(uintptr_t)thunk_11,
    (uint64_t)(uintptr_t)thunk_12, (uint64_t)(uintptr_t)thunk_13, (uint64_t)(uintptr_t)thunk_14, (uint64_t)(uintptr_t)thunk_15,
  };
  GRJIT_NativeTable * table = NULL;
  GRJIT_Result r = grjit_native_table_create(NULL, GLTANG_JIT_ALLOCATOR(exec), &table);
  if (r != GRJIT_OK) {
    return r;
  }
  GRJIT_Type refs[16];
  for (size_t i = 0; i < 16; ++i) {
    refs[i] = GRJIT_TYPE_REF;
  }
  for (uint32_t n = 0; n < 16 && r == GRJIT_OK; ++n) {
    GRJIT_NativeDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.address = calls[n];
    desc.params = refs;
    desc.param_count = (size_t)n + 1u;
    desc.result = GRJIT_TYPE_REF;
    // A host function that re-enters guest code does so under a record of its
    // own (the test natives); the flag is informational to runtime-jit.
    desc.flags = GRJIT_NATIVE_STATUS | GRJIT_NATIVE_REENTERS;
    desc.stack_bytes = GLTANG_JIT_NATIVE_STACK;
    uint32_t id = 0;
    r = grjit_native_table_add(table, &desc, &id);
    if (r == GRJIT_OK && id != GLTANG_JIT_NATIVE_CALL0 + n) {
      r = GRJIT_ERR_INTERNAL;
    }
  }
  if (r == GRJIT_OK) {
    const GRJIT_Type use_params[1] = {GRJIT_TYPE_PTR};
    GRJIT_NativeDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.address = (uint64_t)(uintptr_t)thunk_use;
    desc.params = use_params;
    desc.param_count = 1;
    desc.result = GRJIT_TYPE_REF;
    desc.flags = GRJIT_NATIVE_STATUS;
    desc.stack_bytes = GLTANG_JIT_NATIVE_STACK;
    uint32_t id = 0;
    r = grjit_native_table_add(table, &desc, &id);
    if (r == GRJIT_OK && id != GLTANG_JIT_NATIVE_USE) {
      r = GRJIT_ERR_INTERNAL;
    }
  }
  if (r == GRJIT_OK) {
    const GRJIT_Type attr_params[2] = {GRJIT_TYPE_REF, GRJIT_TYPE_I64};
    GRJIT_NativeDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.address = (uint64_t)(uintptr_t)thunk_attr;
    desc.params = attr_params;
    desc.param_count = 2;
    desc.result = GRJIT_TYPE_REF;
    desc.flags = GRJIT_NATIVE_STATUS;
    desc.stack_bytes = GLTANG_JIT_NATIVE_STACK;
    uint32_t id = 0;
    r = grjit_native_table_add(table, &desc, &id);
    if (r == GRJIT_OK && id != GLTANG_JIT_NATIVE_ATTR) {
      r = GRJIT_ERR_INTERNAL;
    }
  }
  if (r != GRJIT_OK) {
    grjit_native_table_free(table);
    return r;
  }
  *out = table;
  return GRJIT_OK;
}
