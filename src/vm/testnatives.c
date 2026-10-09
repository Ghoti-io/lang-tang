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
 * The engine's test natives (spec-runtime-calls story 9): host functions that
 * reach the engine's internals, which no host can, registered into a library by a
 * test through `gltang_vm_test_add_native` (declared by the test; no header
 * names it). They exist to show the cases a host's opaque native never meets:
 *
 * - `depth()` and `records()`: the native-depth units and activation records in
 *   use while the native runs, which are what the shared wrapper opened.
 * - `echo(x)`, `alloc_ref(x)` and `alloc_n(n)`: a reference returned as it is, one
 *   put in a new array, and a new array of n integers; the last two allocate, so
 *   under a compiled caller they collect with its frames below them.
 * - `reenter(f, a, ...)`: calls the guest function f with the arguments as a nested
 *   activation (AD-23) and returns its value; a pause or unwind inside is the
 *   unwind of this call.
 * - `deopt(x)`: returns x and asks compiled code to leave after the call (the
 *   status the library names `GRJIT_NATIVE_DEOPT`).
 * - `resumable(f)`: f() + f() + f(), the first two of them called by the
 *   interpreter on the native's behalf, with its state in a guest frame (AD-23); it
 *   is reached only through an exit.
 */

#include <ghoti.io/lang-tang/macros.h>

#include <ghoti.io/runtime-core/a/activation.h>
#include "vm_internal.h"

typedef enum {
  TEST_NATIVE_DEPTH = 0,
  TEST_NATIVE_RECORDS,
  TEST_NATIVE_ECHO,
  TEST_NATIVE_ALLOC_REF,
  TEST_NATIVE_ALLOC_N,
  TEST_NATIVE_REENTER,
  TEST_NATIVE_DEOPT,
  TEST_NATIVE_RESUMABLE
} TestNative;

static void return_value(GLTANG_NativeCall * call, GLTANG_Value value) {
  call->set = GLTANG_CALL_VALUE;
  call->value = value;
}

static GLTANG_Value arg(const GLTANG_NativeCall * call, size_t index) {
  return index < call->argc ? call->args[index] : GLTANG_V_NULL;
}

static bool is_array(GLTANG_Value v) {
  return gltang_v_is_kind(v, GLTANG_OBJ_ARRAY);
}

static bool resumable(GLTANG_NativeCall * call) {
  GLTANG_Execution * exec = call->exec;
  if (call->argc != 1u) {
    gltang_call_return_error(call, GLTANG_ERROR_ARGUMENT_COUNT_MISMATCH);
    return true;
  }
  // The state is an array [function, calls made, sum so far], bound to the
  // continuation the interpreter leaves in the caller's frame.
  GLTANG_Value function = GLTANG_V_NULL;
  int64_t made = 0;
  int64_t sum = 0;
  if (call->resume_state) {
    if (!is_array(call->resume_state) || !gltang_v_is_small_int(call->args[0])) {
      gltang_call_return_error(call, GLTANG_ERROR_INVALID_FUNCTION_CALL);
      return true;
    }
    const GLTANG_ArrayObject * state = gltang_vm_array(call->resume_state);
    function = state->store.typed->slots[0];
    made = gltang_v_small_int(state->store.typed->slots[1]) + 1;
    sum = gltang_v_small_int(state->store.typed->slots[2]) + gltang_v_small_int(call->args[0]);
    if (made >= 3) {
      gltang_call_return_integer(call, sum);
      return true;
    }
  }
  else {
    function = call->args[0];
  }
  GLTANG_Value state = gltang_vm_array_new(exec, 3);
  if (!is_array(state)) {
    return_value(call, state);
    return true;
  }
  const GLTANG_Value fill[3] = {function, gltang_v_from_small_int(made), gltang_v_from_small_int(sum)};
  gltang_vm_array_fill(exec, state, fill, 3);
  call->callk_function = function;
  call->callk_state = state;
  return true;
}

static bool test_native(GLTANG_NativeCall * call, void * user) {
  GLTANG_Execution * exec = call->exec;
  switch ((TestNative)(uintptr_t)user) {
    case TEST_NATIVE_DEPTH:
      gltang_call_return_integer(call, (int64_t)grcore_context_depth(exec->context, GRCORE_DEPTH_NATIVE));
      return true;
    case TEST_NATIVE_RECORDS:
      gltang_call_return_integer(call, (int64_t)grcore_activation_count(grcore_context_stack(exec->context)));
      return true;
    case TEST_NATIVE_ECHO:
      return_value(call, arg(call, 0));
      return true;
    case TEST_NATIVE_ALLOC_REF: {
      GLTANG_Value array = gltang_vm_array_new(exec, 1);
      if (is_array(array)) {
        // The argument was copied before anything could collect and the record pins
        // what it names, so it is read from the copy, after the allocation.
        gltang_vm_array_fill(exec, array, &call->args[0], 1);
      }
      return_value(call, array);
      return true;
    }
    case TEST_NATIVE_ALLOC_N: {
      int64_t n = gltang_call_integer(call, 0);
      if (n < 0 || n > 64) {
        gltang_call_return_error(call, GLTANG_ERROR_INVALID_FUNCTION_CALL);
        return true;
      }
      GLTANG_Value array = gltang_vm_array_new(exec, (size_t)n);
      if (is_array(array)) {
        GLTANG_Value items[64];
        for (int64_t i = 0; i < n; ++i) {
          items[i] = gltang_v_from_small_int(i);
        }
        gltang_vm_array_fill(exec, array, items, (size_t)n);
      }
      return_value(call, array);
      return true;
    }
    case TEST_NATIVE_REENTER:
      if (call->argc == 0) {
        gltang_call_return_error(call, GLTANG_ERROR_ARGUMENT_COUNT_MISMATCH);
        return true;
      }
      return_value(call, gltang_vm_call_guest(exec, call->args[0], call->argc - 1u, call->args + 1));
      return true;
    case TEST_NATIVE_DEOPT:
      return_value(call, arg(call, 0));
      call->test_status = GLTANG_NATIVE_STATUS_DEOPT;
      return true;
    case TEST_NATIVE_RESUMABLE:
      return resumable(call);
  }
  return false;
}

GLTANG_Result gltang_vm_test_add_native(GLTANG_Library * library, const char * name, int kind);
GLTANG_Result gltang_vm_test_add_native(GLTANG_Library * library, const char * name, int kind) {
  GLTANG_Result r = gltang_library_add_native(library, name, test_native, (void *)(uintptr_t)kind);
  if (r == GLTANG_OK && kind == (int)TEST_NATIVE_RESUMABLE) {
    r = gltang_library_mark_resumable(library, name);
  }
  return r;
}
