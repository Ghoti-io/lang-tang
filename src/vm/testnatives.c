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
 * test through `gltang_vm_test_add_native` (declared in test_hooks.h, which is not installed and which no host
 * names). They exist to show the cases a host's opaque native never meets:
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
 * - `resumable(f)`: f() + f() + f(), each called by the interpreter on the native's
 *   behalf through the public resumable protocol (gltang_call_guest, AD-23); it is
 *   reached only through an exit. Kept as the engine's regression fixture of the protocol.
 * - `scopes()`: the handle frames open while the native runs (none belongs to a suspended callback).
 * - `after(f)`: calls the guest function f as a nested activation, and returns 1 if
 *   the native is still inside a host function once that has returned (it is: a
 *   native f called must not have cleared the flag the public setters read), else 0.
 */

#include "test_hooks.h"
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
  TEST_NATIVE_RESUMABLE,
  TEST_NATIVE_AFTER,
  TEST_NATIVE_SCOPES
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

/*
 * `resumable(f)`: f() + f() + f(), written on the public protocol (handle.h, library.h): the first
 * entry asks for f() and names the continuation; the continuation adds the value to the sum kept
 * in its state (an array [calls made, sum so far]) and asks again, or answers. It is the engine's
 * regression fixture for the protocol, and the first user of it.
 */
static bool resumable_ask(GLTANG_NativeCall * call, GLTANG_Handle function, GLTANG_Handle state) {
  return gltang_call_guest(call, function, NULL, 0, 0, state) == GLTANG_OK;
}

static bool resumable(GLTANG_NativeCall * call) {
  if (call->argc != 1u) {
    gltang_call_return_error(call, GLTANG_ERROR_ARGUMENT_COUNT_MISMATCH);
    return true;
  }
  GLTANG_Handle function, state, count, sum;
  if (gltang_call_arg(call, 0, &function) != GLTANG_OK || gltang_handle_make_array(call, 2, &state) != GLTANG_OK || gltang_handle_make_integer(call, 0, &count) != GLTANG_OK ||
      gltang_handle_make_integer(call, 0, &sum) != GLTANG_OK || gltang_handle_array_push(call, state, count) != GLTANG_OK || gltang_handle_array_push(call, state, sum) != GLTANG_OK) {
    return false;
  }
  return resumable_ask(call, function, state);
}

static GLTANG_OpStatus resumable_continue(GLTANG_NativeCall * call, void * user, GLTANG_Handle state, GLTANG_Handle result) {
  (void)user;
  GLTANG_Handle function, count, sum;
  if (gltang_call_arg(call, 0, &function) != GLTANG_OK || gltang_handle_kind(call, result) != GLTANG_KIND_INTEGER || gltang_handle_array_get(call, state, 0, &count) != GLTANG_OK ||
      gltang_handle_array_get(call, state, 1, &sum) != GLTANG_OK) {
    gltang_call_return_error(call, GLTANG_ERROR_INVALID_FUNCTION_CALL);
    return GLTANG_OP_ERROR;
  }
  const int64_t made = gltang_handle_integer(call, count) + 1;
  const int64_t total = gltang_handle_integer(call, sum) + gltang_handle_integer(call, result);
  if (made >= 3) {
    gltang_call_return_integer(call, total);
    return GLTANG_OP_OK;
  }
  GLTANG_Handle next, c, t;
  if (gltang_handle_make_array(call, 2, &next) != GLTANG_OK || gltang_handle_make_integer(call, made, &c) != GLTANG_OK || gltang_handle_make_integer(call, total, &t) != GLTANG_OK ||
      gltang_handle_array_push(call, next, c) != GLTANG_OK || gltang_handle_array_push(call, next, t) != GLTANG_OK) {
    return GLTANG_OP_ERROR;
  }
  return resumable_ask(call, function, next) ? GLTANG_OP_PENDING : GLTANG_OP_ERROR;
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
      // One argument: an array holding it. Several: an array holding the first and
      // the last, which is how a call with more arguments than the wrapper's frame
      // holds shows that every one of them was copied and pinned.
      const size_t held = call->argc > 1u ? 2u : 1u;
      GLTANG_Value array = gltang_vm_array_new(exec, held);
      if (is_array(array)) {
        // The arguments were copied before anything could collect and the record pins
        // what they name, so they are read from the copy, after the allocation.
        const GLTANG_Value items[2] = {arg(call, 0), arg(call, call->argc ? call->argc - 1u : 0u)};
        gltang_vm_array_fill(exec, array, items, held);
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
    case TEST_NATIVE_SCOPES:
      // The handle frames open while this native runs: its own, if it has asked for a handle (it has not), and
      // those of any callback that is suspended (none: a suspension keeps no scope).
      gltang_call_return_integer(call, (int64_t)(exec->handles ? exec->handles->frame_count : 0));
      return true;
    case TEST_NATIVE_AFTER: {
      (void)gltang_vm_call_guest(exec, arg(call, 0), 0, NULL);
      gltang_call_return_integer(call, exec->in_host ? 1 : 0);
      return true;
    }
  }
  return false;
}

GLTANG_Result gltang_vm_test_add_native(GLTANG_Library * library, const char * name, int kind) {
  GLTANG_Result r = gltang_library_add_native(library, name, test_native, (void *)(uintptr_t)kind);
  if (r == GLTANG_OK && kind == (int)TEST_NATIVE_RESUMABLE) {
    static const GLTANG_Continuation table[] = {{"resumable_continue", resumable_continue}};
    r = gltang_library_mark_resumable(library, name, table, 1);
  }
  return r;
}
