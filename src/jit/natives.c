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
 * - `PRINT` and `PRINT_CONST`: the interpreter's `gltang_vm_op_print` and
 *   `gltang_vm_print_constant`, so there is one implementation of printing beyond the
 *   common case of a short text or string, which compiled code does first by its fast path
 *   (story 4d, helpers.c); these thunks are its slow path.
 * - The value operations: a string `CONST` (`gltang_vm_constant`), `+` past the
 *   small-integer path (`gltang_vm_op_binary`), `INDEX`, `SLICE` and the `ATTR` of a value
 *   that is not a library (`gltang_vm_op_index`, `gltang_vm_op_slice`,
 *   `gltang_vm_op_attr`), so each is the interpreter's one implementation (a map's `ATTR`
 *   is first tried by its fast path, story 4d, and this thunk is its slow path).
 * - The operations of a raw float (story 4g): its print, its `+` with a string and its cast to
 *   string, given the float's bits (`gltang_vm_print_float`, `gltang_vm_concat_float`,
 *   `gltang_vm_float_to_string`), the interpreter's functions of a float object of that value.
 * - The value-building operations of story 4: a `CAST` (`gltang_vm_op_cast`), the
 *   allocation of an `ARRAY` or a `MAP` literal (`gltang_vm_array_new`,
 *   `gltang_vm_map_new`; its elements are stored after it, by the no-GC fill helpers of
 *   helpers.c) and the copy of a container that `ADOPT` stores (`gltang_vm_op_adopt`).
 *
 * Each returns the value and a status: zero, or `GRJIT_NATIVE_UNWIND` for the
 * `GLTANG_V_UNWIND` an operation returns when a runtime poll ordered the run to
 * stop, or what a test native asked of compiled code (`GRJIT_NATIVE_DEOPT`). A
 * non-zero status leaves compiled code through the chain deopt with the state
 * after the call. Every one of them is a GC point.
 */

#include <ghoti.io/lang-tang/macros.h>

#include "jit_internal.h"

#include <assert.h>
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

/**
 * The execution a template-op thunk serves: compiled code passes it as the first argument, a
 * constant of the code (which is the execution's own, never shared), so the thunk does not look it
 * up in the context's keyed slots (gltang_vm_execution_of, about thirty instructions, which every
 * thunk paid: story 4e). The library-call thunks (`call_n`) still look it up: a call of fifteen
 * arguments and its callee already take all sixteen parameters runtime-jit allows a native.
 */
static GLTANG_Execution * execution_from(void * context, uint64_t execution) {
  // Not checked against the context here: the check is the lookup this saves. Compiled code is
  // made for one execution and entered only by it (its JIT state is the execution's), and the
  // tests that run every template op compiled compare each tier's output, fuel and frames.
  (void)context;
  return (GLTANG_Execution *)(uintptr_t)execution;
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

static GRJIT_NativeResult thunk_use(void * context, uint64_t execution, uint64_t block) {
  GLTANG_Execution * exec = execution_from(context, execution);
  exec->pending_fuel += gltang_opcode_cost_table[GLTANG_OP_USE];
  ++exec->jit_stats.member_loads;
  GLTANG_Value value = gltang_vm_resolve(exec, (const GLTANG_StringBlock *)(uintptr_t)block);
  return finished(exec, value, false);
}

/**
 * The interpreter's `ATTR`: the name made from its constant, then the attribute rule. In the
 * interpreter the container stays on the operand stack for the whole operation: here it is
 * held in the execution's `GLTANG_ROOT_HELD` root while the name is made (a GC point, which may
 * move the container: the copy in this frame is then stale, and the one in the root is updated),
 * and read back from it. The read itself is the interpreter's, given the container as the
 * interpreter gives it, and the call's frame state keeps the container alive across it. The root
 * is one the execution already has: the temp stack held it before story 4b, and its first push
 * allocated the temporaries' storage, which the interpreter's `ATTR` never does, so a memory budget
 * that refused the name on one tier made it on the other. The name is cached in the execution's
 * constants, which the collector reads, once it is made. Nothing between the store and the clear
 * runs guest code, so no other `ATTR` uses the root meanwhile.
 */
static GLTANG_Value attribute(GLTANG_Execution * exec, GLTANG_Value container, uint64_t constant) {
  exec->roots[GLTANG_ROOT_HELD] = container;
  GLTANG_Value name = gltang_vm_constant(exec, (uint32_t)constant);
  container = exec->roots[GLTANG_ROOT_HELD];
  exec->roots[GLTANG_ROOT_HELD] = 0;
  if (name == GLTANG_V_UNWIND) {
    return name;
  }
  return gltang_vm_op_attr(exec, container, name);
}

static GRJIT_NativeResult thunk_attr(void * context, uint64_t execution, uint64_t container, uint64_t constant) {
  GLTANG_Execution * exec = execution_from(context, execution);
  exec->pending_fuel += gltang_opcode_cost_table[GLTANG_OP_ATTR];
  ++exec->jit_stats.member_loads;
  // The interpreter's ATTR stands at the operation whatever its operand, so a poll inside a
  // library's `.name` (a refused allocation of its name or of the member it makes) reads the
  // `ATTR` on both tiers.
  exec->standing = true;
  GLTANG_Value result = attribute(exec, container, constant);
  exec->standing = false;
  return finished(exec, result, false);
}

// The output operations (spec-template-compiled story 1): `PRINT` and `PRINT_CONST`
// call the interpreter's own functions, so output, its rollback, the sink, the pacing
// of a large print and the error a failed print makes are the interpreter's. Neither
// re-enters guest code: the sink runs inside with `in_host` set, as it does for the
// interpreter, and may not re-enter.

/*
 * An operation that may poll partway stands while its function runs, as the interpreter's does:
 * `exec->standing` is set before the call and cleared after it. A poll inside it (its pacing, a
 * refused allocation) first writes the guest frame's poll identity, the compiled caller's frame at
 * the offset compiled code stored before the call (gltang_vm_native_poll), so the identity a poll
 * inside reads is the operation's own. An operation that does not poll writes nothing.
 */

static GRJIT_NativeResult thunk_print(void * context, uint64_t execution, uint64_t value) {
  GLTANG_Execution * exec = execution_from(context, execution);
  exec->pending_fuel += gltang_opcode_cost_table[GLTANG_OP_PRINT];
  ++exec->jit_stats.output_calls;
  exec->standing = true;
  // The operand is in the call's frame state, as it is on the interpreter's operand
  // stack while its PRINT runs: the value is replaced by the result, null or an error.
  GLTANG_Value result = gltang_vm_op_print(exec, value);
  exec->standing = false;
  return finished(exec, result, false);
}

static GRJIT_NativeResult thunk_print_const(void * context, uint64_t execution, uint64_t block) {
  GLTANG_Execution * exec = execution_from(context, execution);
  exec->pending_fuel += gltang_opcode_cost_table[GLTANG_OP_PRINT_CONST];
  ++exec->jit_stats.output_calls;
  exec->standing = true;
  // The interpreter's PRINT_CONST keeps nothing of the value but an unwind.
  GLTANG_Value result = gltang_vm_print_constant(exec, (const GLTANG_StringBlock *)(uintptr_t)block);
  exec->standing = false;
  return finished(exec, result, false);
}

// The value operations (spec-template-compiled story 3). Each charges its operation's fuel,
// as the interpreter charges an operation it runs (an exit before the call has charged
// nothing), and calls the interpreter's own function. Each operand is in the call's frame
// state, as it is on the interpreter's operand stack while the operation runs, so the
// collector keeps it; the vm functions hold what they need across their own allocations
// (the temporaries), as they do for the interpreter, and only `ATTR`, which makes its name
// first, holds its container here. An operand that is a host-defined value never arrives:
// compiled code tests for one before the call and leaves, because the type's operation is a
// host callback, which these helpers do not run (spec-tang-host-extensibility story 10).

static GRJIT_NativeResult thunk_const(void * context, uint64_t execution, uint64_t index, uint64_t site) {
  GLTANG_Execution * exec = execution_from(context, execution);
  exec->pending_fuel += gltang_opcode_cost_table[GLTANG_OP_CONST];
  ++exec->jit_stats.value_calls;
  // The interpreter's CONST: the execution's cached string, made on first use. Only the
  // making records the position (its SYNC), so a cached constant changes nothing.
  GLTANG_Value v = exec->constants[index];
  if (!v) {
    exec->current_function = (uint32_t)(site >> 32);
    exec->current_offset = (uint32_t)site;
    v = gltang_vm_constant(exec, (uint32_t)index);
  }
  return finished(exec, v, false);
}

static GRJIT_NativeResult thunk_add(void * context, uint64_t execution, uint64_t a, uint64_t b) {
  GLTANG_Execution * exec = execution_from(context, execution);
  exec->pending_fuel += gltang_opcode_cost_table[GLTANG_OP_ADD];
  ++exec->jit_stats.value_calls;
  // A long concatenation polls inside (its pacing), at the `+`; a sum of two numbers stands
  // nowhere, as the interpreter's does not (gltang_vm_binary_may_pace, the one predicate).
  if (gltang_vm_binary_may_pace(a, b)) {
    exec->standing = true;
  }
  GLTANG_Value result = gltang_vm_op_binary(exec, GLTANG_OP_ADD, a, b);
  exec->standing = false;
  return finished(exec, result, false);
}

static GRJIT_NativeResult thunk_index(void * context, uint64_t execution, uint64_t container, uint64_t index) {
  GLTANG_Execution * exec = execution_from(context, execution);
  exec->pending_fuel += gltang_opcode_cost_table[GLTANG_OP_INDEX];
  ++exec->jit_stats.value_calls;
  // A string's grapheme is a substring, paced by its bytes.
  exec->standing = true;
  GLTANG_Value result = gltang_vm_op_index(exec, container, index);
  exec->standing = false;
  return finished(exec, result, false);
}

static GRJIT_NativeResult thunk_slice(void * context, uint64_t execution, uint64_t flags, uint64_t container, uint64_t p0, uint64_t p1, uint64_t p2) {
  GLTANG_Execution * exec = execution_from(context, execution);
  exec->pending_fuel += gltang_opcode_cost_table[GLTANG_OP_SLICE];
  ++exec->jit_stats.value_calls;
  exec->standing = true;
  // The parts present, in the order the operand stack holds them (compiled code passes a
  // zero for each absent one, which the slice never reads); it copies them before it allocates.
  const GLTANG_Value parts[3] = {p0, p1, p2};
  GLTANG_Value result = gltang_vm_op_slice(exec, container, (unsigned)flags, parts);
  exec->standing = false;
  return finished(exec, result, false);
}

static GRJIT_NativeResult thunk_value_attr(void * context, uint64_t execution, uint64_t container, uint64_t constant) {
  GLTANG_Execution * exec = execution_from(context, execution);
  exec->pending_fuel += gltang_opcode_cost_table[GLTANG_OP_ATTR];
  ++exec->jit_stats.value_calls;
  // A retag (`.html` and the others) and `.render` poll inside, at the `ATTR`.
  exec->standing = true;
  GLTANG_Value result = attribute(exec, container, constant);
  exec->standing = false;
  return finished(exec, result, false);
}

// The value-building operations (spec-template-compiled story 4), each its operation's fuel and
// the interpreter's own function, as above.

static GRJIT_NativeResult thunk_cast(void * context, uint64_t execution, uint64_t value, uint64_t type) {
  GLTANG_Execution * exec = execution_from(context, execution);
  exec->pending_fuel += gltang_opcode_cost_table[GLTANG_OP_CAST];
  ++exec->jit_stats.value_calls;
  // A cast to string renders its operand, paced by the bytes it renders (gltang_vm_to_string),
  // so it polls inside, at the `CAST`; the other casts neither poll nor stand, on either tier.
  if (type == GLTANG_CAST_TYPE_STRING) {
    exec->standing = true;
  }
  GLTANG_Value result = gltang_vm_op_cast(exec, value, (GLTANG_Cast_Type)type);
  exec->standing = false;
  return finished(exec, result, false);
}

/**
 * The container of a literal, made empty with room for `count` elements (or pairs): the
 * interpreter's allocation, a GC point, and the literal's value even when it is the failure the
 * allocation made. The elements are in the call's frame state and in compiled code's registers,
 * which the stack map names, so the collector keeps them and a moving one updates them; compiled
 * code then stores them with the fill helpers, which do not allocate.
 */
/**
 * The status of a literal's allocation. The call's frame state after it names the container,
 * still unfilled, in the literal's slot (build.c, emit_literal): that is safe only because the one
 * status other than OK is an unwind, which pops the frame without reading or rebuilding it. A
 * status that leaves compiled code with the frame kept (a deopt) would rebuild a frame holding a
 * half-made literal, so none may come from here.
 */
static GRJIT_NativeResult literal_made(const GLTANG_Execution * exec, GLTANG_Value container) {
  GRJIT_NativeResult r = finished(exec, container, false);
  assert((r.status == GRJIT_NATIVE_OK || r.status == GRJIT_NATIVE_UNWIND) && "a literal's allocation may only continue or unwind");
  return r;
}

static GRJIT_NativeResult thunk_array(void * context, uint64_t execution, uint64_t count) {
  GLTANG_Execution * exec = execution_from(context, execution);
  exec->pending_fuel += gltang_opcode_cost_table[GLTANG_OP_ARRAY];
  ++exec->jit_stats.value_calls;
  return literal_made(exec, gltang_vm_array_new(exec, (size_t)count));
}

static GRJIT_NativeResult thunk_map(void * context, uint64_t execution, uint64_t count) {
  GLTANG_Execution * exec = execution_from(context, execution);
  exec->pending_fuel += gltang_opcode_cost_table[GLTANG_OP_MAP];
  ++exec->jit_stats.value_calls;
  return literal_made(exec, gltang_vm_map_new(exec, (size_t)count));
}

/** `ADOPT` of a container (compiled code tests that it is one: of anything else `ADOPT` does nothing, and calls nothing): the interpreter's deep copy, which paces. */
static GRJIT_NativeResult thunk_adopt(void * context, uint64_t execution, uint64_t value) {
  GLTANG_Execution * exec = execution_from(context, execution);
  exec->pending_fuel += gltang_opcode_cost_table[GLTANG_OP_ADOPT];
  ++exec->jit_stats.value_calls;
  exec->standing = true;
  GLTANG_Value result = gltang_vm_op_adopt(exec, value);
  exec->standing = false;
  return finished(exec, result, false);
}

// The operations of a raw float (spec-template-compiled story 4g). Compiled code holds a float it
// computed as a raw double and never boxes it (AD-27); these are given its 64 bits (a native's
// arguments are integer words), and each is the interpreter's function of a float object of that
// value, through the same formatting and the same output path or string builder (text.c, ops.c):
// its fuel, its stand, its counters, its value, as the thunks above for a value word. The float is
// in the call's frame state, raw, where the interpreter's frame holds its box, so a poll inside or
// an unwind after sees it there, and a rebuild boxes it from the pool.

static double float_bits(uint64_t bits) {
  double d;
  memcpy(&d, &bits, sizeof(d));
  return d;
}

static GRJIT_NativeResult thunk_print_float(void * context, uint64_t execution, uint64_t bits) {
  GLTANG_Execution * exec = execution_from(context, execution);
  exec->pending_fuel += gltang_opcode_cost_table[GLTANG_OP_PRINT];
  ++exec->jit_stats.output_calls;
  ++exec->jit_stats.raw_float_texts;
  exec->standing = true;
  GLTANG_Value result = gltang_vm_print_float(exec, float_bits(bits));
  exec->standing = false;
  return finished(exec, result, false);
}

static GRJIT_NativeResult thunk_concat_float(void * context, uint64_t execution, uint64_t s, uint64_t bits, uint64_t float_first) {
  GLTANG_Execution * exec = execution_from(context, execution);
  exec->pending_fuel += gltang_opcode_cost_table[GLTANG_OP_ADD];
  ++exec->jit_stats.value_calls;
  ++exec->jit_stats.raw_float_texts;
  // A string beside: the interpreter's `+` stands (gltang_vm_binary_may_pace), and a long string's
  // copy polls inside, at the `+`.
  exec->standing = true;
  GLTANG_Value result = gltang_vm_concat_float(exec, s, float_bits(bits), float_first != 0);
  exec->standing = false;
  return finished(exec, result, false);
}

static GRJIT_NativeResult thunk_float_string(void * context, uint64_t execution, uint64_t bits) {
  GLTANG_Execution * exec = execution_from(context, execution);
  exec->pending_fuel += gltang_opcode_cost_table[GLTANG_OP_CAST];
  ++exec->jit_stats.value_calls;
  ++exec->jit_stats.raw_float_texts;
  // A cast to string stands, as thunk_cast's does.
  exec->standing = true;
  GLTANG_Value result = gltang_vm_float_to_string(exec, float_bits(bits));
  exec->standing = false;
  return finished(exec, result, false);
}

/** Registers one native that takes `count` parameters of `params` and has a status: its id must be `expected`. */
static GRJIT_Result add_native(GRJIT_NativeTable * table, uint64_t address, const GRJIT_Type * params, size_t count, uint32_t expected) {
  // The execution first (execution_from), then the operation's own parameters.
  GRJIT_Type with_execution[8];
  assert(count + 1u <= sizeof(with_execution) / sizeof(with_execution[0]) && "a template op's native takes at most seven parameters");
  with_execution[0] = GRJIT_TYPE_PTR;
  for (size_t k = 0; k < count; ++k) {
    with_execution[1u + k] = params[k];
  }
  GRJIT_NativeDesc desc;
  memset(&desc, 0, sizeof(desc));
  desc.address = address;
  desc.params = with_execution;
  desc.param_count = count + 1u;
  desc.result = GRJIT_TYPE_REF;
  desc.flags = GRJIT_NATIVE_STATUS;
  desc.stack_bytes = GLTANG_JIT_NATIVE_STACK;
  uint32_t id = 0;
  GRJIT_Result r = grjit_native_table_add(table, &desc, &id);
  return r == GRJIT_OK && id != expected ? GRJIT_ERR_INTERNAL : r;
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
    const GRJIT_Type use_params[2] = {GRJIT_TYPE_PTR, GRJIT_TYPE_PTR};
    GRJIT_NativeDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.address = (uint64_t)(uintptr_t)thunk_use;
    desc.params = use_params;
    desc.param_count = 2;
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
    const GRJIT_Type attr_params[3] = {GRJIT_TYPE_PTR, GRJIT_TYPE_REF, GRJIT_TYPE_I64};
    GRJIT_NativeDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.address = (uint64_t)(uintptr_t)thunk_attr;
    desc.params = attr_params;
    desc.param_count = 3;
    desc.result = GRJIT_TYPE_REF;
    desc.flags = GRJIT_NATIVE_STATUS;
    desc.stack_bytes = GLTANG_JIT_NATIVE_STACK;
    uint32_t id = 0;
    r = grjit_native_table_add(table, &desc, &id);
    if (r == GRJIT_OK && id != GLTANG_JIT_NATIVE_ATTR) {
      r = GRJIT_ERR_INTERNAL;
    }
  }
  if (r == GRJIT_OK) {
    const GRJIT_Type print_params[1] = {GRJIT_TYPE_REF};
    r = add_native(table, (uint64_t)(uintptr_t)thunk_print, print_params, 1, GLTANG_JIT_NATIVE_PRINT);
  }
  if (r == GRJIT_OK) {
    const GRJIT_Type print_const_params[1] = {GRJIT_TYPE_PTR};
    r = add_native(table, (uint64_t)(uintptr_t)thunk_print_const, print_const_params, 1, GLTANG_JIT_NATIVE_PRINT_CONST);
  }
  if (r == GRJIT_OK) {
    const GRJIT_Type const_params[2] = {GRJIT_TYPE_I64, GRJIT_TYPE_I64};
    r = add_native(table, (uint64_t)(uintptr_t)thunk_const, const_params, 2, GLTANG_JIT_NATIVE_CONST);
  }
  if (r == GRJIT_OK) {
    const GRJIT_Type add_params[2] = {GRJIT_TYPE_REF, GRJIT_TYPE_REF};
    r = add_native(table, (uint64_t)(uintptr_t)thunk_add, add_params, 2, GLTANG_JIT_NATIVE_ADD);
  }
  if (r == GRJIT_OK) {
    const GRJIT_Type index_params[2] = {GRJIT_TYPE_REF, GRJIT_TYPE_REF};
    r = add_native(table, (uint64_t)(uintptr_t)thunk_index, index_params, 2, GLTANG_JIT_NATIVE_INDEX);
  }
  if (r == GRJIT_OK) {
    const GRJIT_Type slice_params[5] = {GRJIT_TYPE_I64, GRJIT_TYPE_REF, GRJIT_TYPE_REF, GRJIT_TYPE_REF, GRJIT_TYPE_REF};
    r = add_native(table, (uint64_t)(uintptr_t)thunk_slice, slice_params, 5, GLTANG_JIT_NATIVE_SLICE);
  }
  if (r == GRJIT_OK) {
    const GRJIT_Type value_attr_params[2] = {GRJIT_TYPE_REF, GRJIT_TYPE_I64};
    r = add_native(table, (uint64_t)(uintptr_t)thunk_value_attr, value_attr_params, 2, GLTANG_JIT_NATIVE_VALUE_ATTR);
  }
  if (r == GRJIT_OK) {
    const GRJIT_Type cast_params[2] = {GRJIT_TYPE_REF, GRJIT_TYPE_I64};
    r = add_native(table, (uint64_t)(uintptr_t)thunk_cast, cast_params, 2, GLTANG_JIT_NATIVE_CAST);
  }
  if (r == GRJIT_OK) {
    const GRJIT_Type count_params[1] = {GRJIT_TYPE_I64};
    r = add_native(table, (uint64_t)(uintptr_t)thunk_array, count_params, 1, GLTANG_JIT_NATIVE_ARRAY);
    if (r == GRJIT_OK) {
      r = add_native(table, (uint64_t)(uintptr_t)thunk_map, count_params, 1, GLTANG_JIT_NATIVE_MAP);
    }
  }
  if (r == GRJIT_OK) {
    const GRJIT_Type adopt_params[1] = {GRJIT_TYPE_REF};
    r = add_native(table, (uint64_t)(uintptr_t)thunk_adopt, adopt_params, 1, GLTANG_JIT_NATIVE_ADOPT);
  }
  if (r == GRJIT_OK) {
    const GRJIT_Type bits_params[1] = {GRJIT_TYPE_I64};
    r = add_native(table, (uint64_t)(uintptr_t)thunk_print_float, bits_params, 1, GLTANG_JIT_NATIVE_PRINT_FLOAT);
    if (r == GRJIT_OK) {
      const GRJIT_Type concat_params[3] = {GRJIT_TYPE_REF, GRJIT_TYPE_I64, GRJIT_TYPE_I64};
      r = add_native(table, (uint64_t)(uintptr_t)thunk_concat_float, concat_params, 3, GLTANG_JIT_NATIVE_CONCAT_FLOAT);
    }
    if (r == GRJIT_OK) {
      r = add_native(table, (uint64_t)(uintptr_t)thunk_float_string, bits_params, 1, GLTANG_JIT_NATIVE_FLOAT_STRING);
    }
  }
  if (r != GRJIT_OK) {
    grjit_native_table_free(table);
    return r;
  }
  *out = table;
  return GRJIT_OK;
}
