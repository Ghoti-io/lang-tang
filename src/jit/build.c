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
 * Bytecode to runtime-jit IR, and the compile.
 *
 * A compiled function is callable (AD-28): it takes the callee's parameters as
 * references and one hidden trailing flag, keeps one virtual register per local
 * and per operand-stack position, and runs "after the entry poll": bytecode
 * index 1, an empty operand stack. The flag is 1 from the interpreter, which has
 * made the function's entry poll itself, and 0 from a compiled call, whose callee
 * makes its own; the prologue branches on it, so one body serves both.
 *
 * What it inlines is the cheap and the exact: small-integer arithmetic (`/` and `%`
 * too) and comparisons, float arithmetic and comparisons and the casts to int and to
 * float (below), booleans, locals and globals, function values, jumps, `POLL`,
 * `LINE`, `RET`, `CALL` of a function the declaration scan can name, and the
 * library: the load of a `use`d path or a library's `.name` and the `CALL` of a
 * library native (a host function), each a call to the engine's own operation
 * (natives.c) with no exit; the output operations, `PRINT` and `PRINT_CONST` as calls of
 * the interpreter's print through the same table (after a fast path for a short text or
 * string, story 4d, a second implementation of that case) and `CLEAR_RESULT` as a call that
 * is no GC point; the value operations, a string `CONST`, `+` past the small-integer path,
 * `INDEX`, `SLICE` and the `ATTR` of any other value (after a fast path for a map, story 4d),
 * as calls of the interpreter's own functions through the same table, behind a test that no operand is a host-defined value (an exit:
 * the type's operation is a host callback, which no compiled helper runs yet); the
 * value-building operations, a `CAST` the inline path does not do, the `ARRAY` and `MAP`
 * literals (an allocating call, then no-GC fills) and `ADOPT` of a container, the same way,
 * with a cast to int of a float constant folded (or, out of range, an exit); and the end of a statement and of a template, `SET_RESULT` and `DISCARD` as calls
 * that are no GC point, `STORE_GLOBAL` as the interpreter's plain store, and `HALT` as a
 * return of the halted word (jit.h), after which the interpreter runs `HALT`'s body. Each
 * step mirrors the interpreter's own inline path, so a guard that holds computes
 * the word the interpreter would. For everything else, and for an operand that
 * is not what the inline path takes, the function leaves through a guard (a
 * deoptimization exit) with the interpreter's exact frame as its frame state,
 * and the interpreter re-executes the operation whole.
 *
 * A frame state is exactly the guest frame, in its own slot order: the function,
 * `pc` and `sp` are constants, the flags word is a register holding the
 * activation's depth, a local is its register, an operand slot below the depth is
 * its register and a slot above is a register that holds 0 typed as a reference
 * (so it reads as the zero value slot the interpreter's frame holds, and is not
 * `DEAD`, which a read takes as a raw word).
 *
 * A float constant is held raw. Each local and each operand position has a kind at
 * every point of the code, `REF` (a value word, as above) or `F64` (a raw double),
 * and a slot that is ever raw has a second, `F64` register beside its `REF` one;
 * a frame state names exactly one of the two. The sources of a raw double are a
 * float `CONST` (its bits, as the interpreter's constant has them), a `CAST` to float
 * and an arithmetic result with a raw operand; `LOAD_LOCAL`, `STORE_LOCAL`, `DUP`,
 * `POP` and the jumps carry a kind unchanged. An instruction that reads a raw operand
 * and is not one of the operations below is an unconditional exit with the operand in
 * its frame state, so compiled code never boxes: a rebuild does, once, from boxes
 * made ahead of time (AD-27, boxes.c). Where the ways into a jump target disagree on
 * a slot's kind, every raw source of the function becomes an exit and the analysis is
 * run again, so the function is then compiled as it was before floats were carried.
 *
 * Float operations (story 7): `+ - * /`, unary minus and the six comparisons compute
 * with doubles when an operand is statically raw (the static kinds decide what is a
 * float; the run-time type only decides whether a guard holds, so two `REF` operands
 * stay the small-integer instruction and exit on a float). The other operand is read as
 * a double by `unbox` (a small integer is converted with `(double)`, a float object is
 * read after a pointer test and a `kind` test, anything else is a guard exit), once per
 * use and never kept. A float result is a raw double; a comparison is a boolean word; an
 * integer result is a small-integer word. Every case the interpreter turns into an error
 * or a heap allocation (a zero divisor, `%` on a float, a NaN or out-of-range float to
 * int, a result outside the small-integer range, a boxed integer, a non-number) is a guard
 * that exits before the operation's effect, and every guard of an operation comes before
 * the result is written to its slot register, so a frame state still names the operands as
 * the interpreter's frame holds them. Division and truncation are written with the guards
 * runtime-jit's verifier requires, in the shapes it recognizes (`guard_nonzero`, the
 * minimum-over-minus-one pair, `trunc_small`).
 *
 * Fuel is the same on every tier: every executed bytecode costs
 * `gltang_opcode_cost_table[op]`. Compiled code adds it directly to the
 * execution's `pending_fuel`, one add for each straight run of operations, before
 * anything that can observe it or leave: a guard, a call, a poll (which also
 * flushes it to the context, as the interpreter's `POLL` does), a branch and
 * `RET`. A guard is tested before its operation's cost is added, so an operation
 * that deoptimizes has not been charged and the interpreter charges it when it
 * runs it. A `CALL`'s own cost is added by the push hook once the push succeeded.
 */

#include <ghoti.io/lang-tang/macros.h>

#include "jit_internal.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <ghoti.io/cutil/memory.h>

#define H GLTANG_FRAME_HEADER

/** @brief The temporaries: all plain integers, reused by every operation. */
enum { TMP_A, TMP_B, TMP_X, TMP_Y, TMP_R, TMP_C, TMP_D, TMP_U, TMP_F, TMP_COUNT };

/** A position of the operand stack whose producing instruction is not one. */
#define PROD_NONE 0xFFFFFFFFu
/** A declared slot with no declaration found, and one with more than one. */
#define DECL_NONE 0xFFFFFFFFu
#define DECL_MANY 0xFFFFFFFEu

typedef struct Ctx {
  GLTANG_Execution * exec;
  const GLTANG_Program * program;
  const GLTANG_Function * fn;
  uint32_t program_index;
  uint32_t function_index;
  uint64_t fword;
  uint32_t params;                ///< parameter_count
  uint32_t lc;                    ///< local_count
  uint32_t ms;                    ///< max_stack
  uint32_t slots;                 ///< frame_slots: what a frame state describes
  GRJIT_Builder * b;
  GRJIT_Result err;               ///< The first builder error, sticky.
  GRJIT_VReg local[GLTANG_JIT_MAX_SLOTS];
  GRJIT_VReg stack[GLTANG_JIT_MAX_SLOTS];
  GRJIT_VReg local_f[GLTANG_JIT_MAX_SLOTS];   ///< The `F64` register beside a local's `REF` one, where the local is ever raw.
  GRJIT_VReg stack_f[GLTANG_JIT_MAX_SLOTS];   ///< The same for an operand position.
  GRJIT_VReg flag;                ///< The hidden parameter: 1 from the interpreter's entry, 0 from a compiled call.
  GRJIT_VReg zero;                ///< 0, typed as a reference: what a dead operand slot reads as.
  GRJIT_VReg flags;               ///< The frame's flags word (the activation's depth), a plain word.
  GRJIT_VReg ep;                  ///< The execution's address, a plain pointer.
  GRJIT_VReg act;                 ///< The execution's activation, for the prologue.
  GRJIT_VReg gp;                  ///< The globals array, for a global load or store.
  GRJIT_VReg halted;              ///< The halted word a `HALT` returns (jit.h).
  GRJIT_VReg built;               ///< The container a literal's native made, held while its elements are stored (story 4).
  bool need_built;                ///< The function has a literal that is compiled, so declares it.
  GRJIT_VReg tmp[TMP_COUNT];
  GRJIT_VReg tmp_f[2];            ///< The `F64` temporaries of a float operation: an unboxed operand lives in one, for that operation only.
  bool need_tmp_f;                ///< The function has a float operation or a cast to int, so declares them.
  GRJIT_FrameSlot state[GLTANG_JIT_MAX_SLOTS];
  GRJIT_FrameSlot state2[GLTANG_JIT_MAX_SLOTS];
  uint64_t pending_cost;          ///< Fuel of the operations emitted since the last add to memory.
  bool open;                      ///< The current block has no terminator yet.
  bool oom;                       ///< An analysis allocation failed.
  GRJIT_BlockId entry_poll;       ///< The prologue's branch target when the flag is 0.
  GRJIT_BlockId guard_to;         ///< While `guard_to_on`: where a failed guard goes instead of exiting (the slow path of `+`).
  bool guard_to_on;

  // The analysis.
  int32_t * depth;                ///< Per bytecode index: the operand-stack depth there, or -1 if unreachable.
  bool * leader;
  bool * target;                  ///< A jump lands here (found by a scan of the code, before the analysis).
  bool * queued;
  GRJIT_BlockId * block;
  uint32_t * work;
  uint32_t ** prod_at;            ///< For a jump target: the producing instruction of each operand position, merged over every way in.
  uint32_t * cur;                 ///< The same, along the straight run being walked.
  int32_t * call_k;               ///< Per index of a CALL: the function it calls, or -1 (an exit).
  const GLTANG_LibraryMember ** call_member;  ///< Per index of a CALL: the library native it calls, or NULL.
  uint32_t * attr_src;            ///< Per index of an ATTR: the instruction that produced the operand it reads.
  uint8_t * str_side;             ///< Per index of an ADD: bit 0, its left operand is a string constant (the CONST that pushed it); bit 1, its right.
  uint32_t * cast_src;            ///< Per index of a CAST to int: the float constant its operand is (the CONST that pushed it, raw), or PROD_NONE.
  bool * load_ok;                 ///< Per index of a USE or ATTR: compiled as a member load.
  uint8_t * end_kind;             ///< Per index reached by the walk: how the walk ended there, if it did (END_*).
  bool back_branch;               ///< A reachable jump goes to its own index or an earlier one: the code may loop.
  bool * declined;                ///< Per index: a library call or member load that natives being unavailable left as an exit.
  uint32_t * global_use;          ///< Per global: the path constant its `use` stores, DECL_NONE or DECL_MANY.
  uint32_t * local_use;           ///< The same for this function's locals.
  uint8_t * expected_sites;       ///< One bit per index where the unconditional exit is the designed one: a raw float boxed for its consumer.
  uint8_t * native_sites;         ///< One bit per index where a native call (a library call or a member load) is emitted.
  bool natives_on;
  bool * flowed;                  ///< Per index of a CALL: the walk went on past it as a call.
  bool * force_exit;              ///< Per index of a CALL: after a merge made its callee unnamable.
  GRJIT_VReg jitp;                ///< This execution's JIT state, read once at entry from the environment.
  GRJIT_VReg callee_slot;         ///< The callee's entry slot, read from the environment's tables at each call.
  GRJIT_VReg callee_tbl;          ///< The tables on the way to it.
  uint32_t * global_fn;           ///< Per global: the function its declaration stores, DECL_NONE or DECL_MANY.
  uint32_t * local_fn;            ///< The same for this function's locals.
  uint8_t * call_sites;           ///< One bit per index where a compiled call is emitted.
  bool calls_on;

  // The kinds (raw floats). `lk` and `sk` are the kind of each local and each
  // operand position at the point being walked or emitted; `kind_at` is the
  // merged state at a jump target.
  uint8_t lk[GLTANG_JIT_MAX_SLOTS];
  uint8_t sk[GLTANG_JIT_MAX_SLOTS];
  bool ever_local[GLTANG_JIT_MAX_SLOTS];      ///< The local is raw somewhere in the code that is compiled.
  bool ever_stack[GLTANG_JIT_MAX_SLOTS];
  uint8_t ** kind_at;             ///< For a jump target: the kinds of the locals, then of the operand positions below its depth.
  bool raw_off;                   ///< Every raw source is an exit (a merge disagreed on a kind).
  bool conflict;                  ///< The walk found a merge whose ways in disagree on a kind.
} Ctx;

enum { KIND_REF = 0, KIND_F64 = 1 };

/** How the walk of the analysis ended at an instruction (`end_kind`). */
enum { END_NONE = 0, END_EXPECTED, END_REAL, END_RETURN };

#define B(call) \
  do { \
    if (c->err == GRJIT_OK) { \
      c->err = (call); \
    } \
  } while (0)

static GRJIT_Operand V(GRJIT_VReg r) { return grjit_operand_vreg(r); }
static GRJIT_Operand I(int64_t n) { return grjit_operand_imm(n); }

// ---------------------------------------------------------------------------
// What is compiled
// ---------------------------------------------------------------------------

static bool small_int_constant(const Ctx * c, uint32_t index, int64_t * out) {
  if (index >= c->program->constant_count) {
    return false;
  }
  const GLTANG_Const * k = &c->program->constants[index];
  if (k->kind != GLTANG_CONST_INTEGER || k->integer < GLTANG_SMALL_INT_MIN || k->integer > GLTANG_SMALL_INT_MAX) {
    return false;
  }
  *out = k->integer;
  return true;
}

/** A float constant: the interpreter's double, as the 64 bits it has (never through arithmetic). */
static bool float_constant(const Ctx * c, uint32_t index, int64_t * bits) {
  if (index >= c->program->constant_count || c->program->constants[index].kind != GLTANG_CONST_FLOAT) {
    return false;
  }
  uint64_t u;
  memcpy(&u, &c->program->constants[index].number, sizeof(u));
  *bits = (int64_t)u;
  return true;
}

static bool use_load_ok(const Ctx * c, uint32_t constant);

/** A string constant, as `PRINT_CONST` names one: its block is what the interpreter prints. */
static bool string_constant(const Ctx * c, uint32_t index) {
  return index < c->program->constant_count && c->program->constants[index].kind == GLTANG_CONST_STRING && c->program->constants[index].block;
}

/** Whether the instruction that produced an operand is the `CONST` of a string. */
static bool string_producer(const Ctx * c, uint32_t producer) {
  return producer != PROD_NONE && producer < c->fn->code_count && GLTANG_INSTRUCTION_OP(c->fn->code[producer]) == GLTANG_OP_CONST
      && string_constant(c, GLTANG_INSTRUCTION_A(c->fn->code[producer]));
}

/** Whether an instruction other than CALL is compiled inline, or is a deoptimization exit. */
static bool inline_op(const Ctx * c, GLTANG_Opcode op, uint32_t a) {
  int64_t unused;
  switch (op) {
    case GLTANG_OP_POLL:
    case GLTANG_OP_LINE:
    case GLTANG_OP_POP:
    case GLTANG_OP_DUP:
    case GLTANG_OP_NULL:
    case GLTANG_OP_TRUE:
    case GLTANG_OP_FALSE:
    case GLTANG_OP_LOAD_LOCAL:
    case GLTANG_OP_STORE_LOCAL:
    case GLTANG_OP_NEG:
    case GLTANG_OP_NOT:
    case GLTANG_OP_ADD:
    case GLTANG_OP_SUB:
    case GLTANG_OP_MUL:
    case GLTANG_OP_DIV:
    case GLTANG_OP_MOD:
    case GLTANG_OP_LT:
    case GLTANG_OP_LE:
    case GLTANG_OP_GT:
    case GLTANG_OP_GE:
    case GLTANG_OP_EQ:
    case GLTANG_OP_NE:
    case GLTANG_OP_JMP:
    case GLTANG_OP_JMP_FALSE:
    case GLTANG_OP_JMP_TRUE:
    case GLTANG_OP_AND:
    case GLTANG_OP_OR:
    case GLTANG_OP_RET:
      return true;
    case GLTANG_OP_CAST:
      // To int and to float inline; a cast to float makes a raw double, which is a raw source.
      // To bool and to string (story 4) are calls of the interpreter's cast, through the table.
      return a == GLTANG_CAST_TYPE_INTEGER || (a == GLTANG_CAST_TYPE_FLOAT && !c->raw_off)
          || ((a == GLTANG_CAST_TYPE_BOOLEAN || a == GLTANG_CAST_TYPE_STRING) && c->natives_on);
    case GLTANG_OP_USE:
      return c->natives_on && use_load_ok(c, a);
    // The output operations are calls of the interpreter's own functions: the two
    // prints through the native table (so only where natives can be called), each after
    // its fast path (story 4d, emit_print), the clearing of the statement result as a call
    // that is no GC point.
    case GLTANG_OP_PRINT:
      return c->natives_on;
    case GLTANG_OP_PRINT_CONST:
      // compile.c emits PRINT_CONST only with const_string's index, so the test is a fail-safe.
      return c->natives_on && string_constant(c, a);
    case GLTANG_OP_CLEAR_RESULT:
      return true;
    // The statement ends (spec-template-compiled story 2): the statement result and the
    // discard of a value are no-GC calls of the interpreter's own functions, a global
    // store is the interpreter's plain store, and `HALT` is a return with the halted word,
    // after which the interpreter runs `HALT`'s body (jit.h).
    case GLTANG_OP_SET_RESULT:
    case GLTANG_OP_DISCARD:
    case GLTANG_OP_HALT:
      return true;
    case GLTANG_OP_STORE_GLOBAL:
    case GLTANG_OP_LOAD_GLOBAL:
      return a < c->program->global_count;
    case GLTANG_OP_FUNC:
      return a < c->program->function_count;
    case GLTANG_OP_CONST:
      // A string constant is the execution's cached heap string, made on its first use:
      // whether it is made yet is not known here, so it is a call of the interpreter's
      // `gltang_vm_constant` (story 3), through the table.
      return small_int_constant(c, a, &unused) || (!c->raw_off && float_constant(c, a, &unused)) || (c->natives_on && string_constant(c, a));
    // The value operations (spec-template-compiled story 3): calls of the interpreter's own
    // index and slice, through the table. `ATTR` and `+` are decided where they are emitted
    // (a library's member, a map's fast path of story 4d, the small-integer path).
    case GLTANG_OP_INDEX:
    case GLTANG_OP_SLICE:
      return c->natives_on;
    // The value-building operations (story 4): an array or a map literal is its native's
    // allocation and then the no-GC fill of its elements, and `ADOPT` of a container is the
    // interpreter's copy through the table (of anything else, nothing at all).
    case GLTANG_OP_ARRAY:
    case GLTANG_OP_MAP:
    case GLTANG_OP_ADOPT:
      return c->natives_on;
    default:
      return false;
  }
}

/** How a cast to int of a float constant is compiled (`cast_src`): not one, folded, or an exit. */
enum { CAST_FOLD_NONE = 0, CAST_FOLD_IN, CAST_FOLD_OUT };

/**
 * A cast to int whose operand is a float constant, as it stands (story 4): inside the
 * small-integer range it is folded to the integer the interpreter's truncation makes; outside it
 * (and a NaN) it is an unconditional exit, and the interpreter makes the boxed integer or the
 * error. Either way no `TRUNC_S` of a `CONST` reaches runtime-jit's verifier, which judges one as
 * it stands, ignoring the guards before it, and refused the whole function for an out-of-range one.
 */
static int constant_cast(const Ctx * c, uint32_t i, int64_t * out) {
  int64_t bits = 0;
  if (!c->cast_src || c->cast_src[i] == PROD_NONE || !float_constant(c, GLTANG_INSTRUCTION_A(c->fn->code[c->cast_src[i]]), &bits)) {
    return CAST_FOLD_NONE;
  }
  double v;
  uint64_t u = (uint64_t)bits;
  memcpy(&v, &u, sizeof(v));
  // The bounds trunc_small guards: [-2^59, 2^59). A NaN fails both.
  if (!(v >= -0x1p59 && v < 0x1p59)) {
    return CAST_FOLD_OUT;
  }
  *out = (int64_t)v;
  return CAST_FOLD_IN;
}

// ---------------------------------------------------------------------------
// Kinds
// ---------------------------------------------------------------------------

static void set_stack_kind(Ctx * c, uint32_t position, uint8_t kind) {
  c->sk[position] = kind;
  if (kind == KIND_F64) {
    c->ever_stack[position] = true;
  }
}

static void set_local_kind(Ctx * c, uint32_t local, uint8_t kind) {
  c->lk[local] = kind;
  if (kind == KIND_F64) {
    c->ever_local[local] = true;
  }
}

/**
 * Whether the instruction at depth `d` reads an operand that is raw. A carrier
 * (`LOAD_LOCAL`, `STORE_LOCAL`, `DUP`, `POP`) does not read it, it moves it; every
 * other instruction that does is an unconditional exit with the operand in its frame
 * state, and the interpreter re-executes it.
 */
static bool reads_raw(const Ctx * c, uint32_t i, GLTANG_Opcode op, uint32_t a, int32_t d) {
  switch (op) {
    case GLTANG_OP_NOT:
    case GLTANG_OP_JMP_FALSE:
    case GLTANG_OP_JMP_TRUE:
    case GLTANG_OP_AND:
    case GLTANG_OP_OR:
    case GLTANG_OP_RET:
    case GLTANG_OP_ATTR:
    // A raw float printed is boxed by the exit and printed by the interpreter, so its
    // text is the interpreter's and compiled code never boxes (AD-27).
    case GLTANG_OP_PRINT:
    // The same for a raw float stored in a global, made the statement result or
    // discarded: each keeps a value word where the interpreter keeps one, so the float is
    // boxed by the exit (AD-27), and the interpreter runs the operation.
    case GLTANG_OP_STORE_GLOBAL:
    case GLTANG_OP_SET_RESULT:
    case GLTANG_OP_DISCARD:
      return d >= 1 && c->sk[d - 1] == KIND_F64;
    // `%` is integers only: with a raw operand the interpreter reports "not supported".
    case GLTANG_OP_MOD:
      return d >= 2 && (c->sk[d - 2] == KIND_F64 || c->sk[d - 1] == KIND_F64);
    // A cast to int or to float reads a raw operand; to bool and to string with one is the designed
    // box exit, and the interpreter makes the text (a raw-float to-string is not compiled).
    case GLTANG_OP_CAST:
      return d >= 1 && c->sk[d - 1] == KIND_F64 && a != GLTANG_CAST_TYPE_INTEGER && a != GLTANG_CAST_TYPE_FLOAT;
    // An index or a slice of a raw float, or by one: the float is boxed by the exit and the
    // interpreter runs the operation (its error), so the helper is never given one raw.
    case GLTANG_OP_INDEX:
      return d >= 2 && (c->sk[d - 2] == KIND_F64 || c->sk[d - 1] == KIND_F64);
    case GLTANG_OP_SLICE: {
      const int32_t parts = (int32_t)((a & 1u) + ((a >> 1) & 1u) + ((a >> 2) & 1u));
      for (int32_t p = d - 1 - parts > 0 ? d - 1 - parts : 0; p < d; ++p) {
        if (c->sk[p] == KIND_F64) {
          return true;
        }
      }
      return false;
    }
    // A raw float stored by `ADOPT` or as an element of a literal (story 4) is boxed by the exit
    // and the interpreter runs the operation, so the helpers are only ever given value words.
    case GLTANG_OP_ADOPT:
      return d >= 1 && c->sk[d - 1] == KIND_F64;
    case GLTANG_OP_ARRAY:
    case GLTANG_OP_MAP: {
      const int64_t n = op == GLTANG_OP_MAP ? 2 * (int64_t)a : (int64_t)a;
      if ((int64_t)d < n) {
        return false;  // a malformed depth: the walk then refuses the instruction (GLTANG_ERR_INTERNAL)
      }
      for (int64_t p = (int64_t)d - n; p < d; ++p) {
        if (c->sk[p] == KIND_F64) {
          return true;
        }
      }
      return false;
    }
    // `+` with a raw float on one side and a string constant on the other is a concatenation,
    // which converts the float to text: the designed box exit, and the interpreter makes the
    // text (a raw-float concatenation is planning/unboxed-float-output.md's). With any other
    // operand beside a raw float it is float arithmetic, whose unbox is a guard (a string from
    // a variable fails it, a real exit).
    case GLTANG_OP_ADD:
      if (d >= 2 && c->str_side) {
        return (c->sk[d - 2] == KIND_F64 && (c->str_side[i] & 2u)) || (c->sk[d - 1] == KIND_F64 && (c->str_side[i] & 1u));
      }
      return false;
    // These read a raw operand and compute with it (float arithmetic, a comparison, a negation).
    case GLTANG_OP_NEG:
    case GLTANG_OP_SUB:
    case GLTANG_OP_MUL:
    case GLTANG_OP_DIV:
    case GLTANG_OP_LT:
    case GLTANG_OP_LE:
    case GLTANG_OP_GT:
    case GLTANG_OP_GE:
    case GLTANG_OP_EQ:
    case GLTANG_OP_NE:
      return false;
    case GLTANG_OP_CALL:
      for (int32_t p = d - 1 - (int32_t)a; p >= 0 && p < d; ++p) {
        if (c->sk[p] == KIND_F64) {
          return true;
        }
      }
      return false;
    // The instructions that read no operand of the stack, or only move one: named, so
    // that an opcode added later is not on this list by omission.
    case GLTANG_OP_POLL:
    case GLTANG_OP_LINE:
    case GLTANG_OP_POP:
    case GLTANG_OP_DUP:
    case GLTANG_OP_NULL:
    case GLTANG_OP_TRUE:
    case GLTANG_OP_FALSE:
    case GLTANG_OP_CONST:
    case GLTANG_OP_LOAD_LOCAL:
    case GLTANG_OP_STORE_LOCAL:
    case GLTANG_OP_LOAD_GLOBAL:
    case GLTANG_OP_FUNC:
    case GLTANG_OP_USE:
    case GLTANG_OP_JMP:
    case GLTANG_OP_PRINT_CONST:
    case GLTANG_OP_CLEAR_RESULT:
    // `HALT` reads no operand: whatever is on the stack goes with the frame.
    case GLTANG_OP_HALT:
      return false;
    default:
      // Fail safe: any other opcode, which the compiler does not know the operands of,
      // is taken to read every operand, so it is an exit if any is raw.
      for (int32_t p = 0; p < d; ++p) {
        if (c->sk[p] == KIND_F64) {
          return true;
        }
      }
      return false;
  }
}

/** The effect on the kinds of an instruction that flows on (an exit changes nothing, since nothing follows it). */
static bool apply_kinds(Ctx * c, GLTANG_Opcode op, uint32_t a, int32_t d) {
  int64_t unused;
  switch (op) {
    case GLTANG_OP_DUP:
      set_stack_kind(c, (uint32_t)d, c->sk[d - 1]);
      break;
    case GLTANG_OP_NULL:
    case GLTANG_OP_TRUE:
    case GLTANG_OP_FALSE:
    case GLTANG_OP_FUNC:
    case GLTANG_OP_LOAD_GLOBAL:
    case GLTANG_OP_USE:
      set_stack_kind(c, (uint32_t)d, KIND_REF);
      break;
    case GLTANG_OP_CONST:
      set_stack_kind(c, (uint32_t)d, !c->raw_off && float_constant(c, a, &unused) ? KIND_F64 : KIND_REF);
      break;
    case GLTANG_OP_LOAD_LOCAL:
      set_stack_kind(c, (uint32_t)d, c->lk[a]);
      break;
    case GLTANG_OP_STORE_LOCAL:
      set_local_kind(c, a, c->sk[d - 1]);
      break;
    case GLTANG_OP_NOT:
    case GLTANG_OP_ATTR:
    case GLTANG_OP_PRINT:  // the operand is replaced by the print's value, null or an error
      set_stack_kind(c, (uint32_t)d - 1u, KIND_REF);
      break;
    case GLTANG_OP_NEG:
      set_stack_kind(c, (uint32_t)d - 1u, c->sk[d - 1] == KIND_F64 ? KIND_F64 : KIND_REF);
      break;
    case GLTANG_OP_CAST:
      if (a == GLTANG_CAST_TYPE_FLOAT) {
        set_stack_kind(c, (uint32_t)d - 1u, KIND_F64);
      }
      else {
        set_stack_kind(c, (uint32_t)d - 1u, KIND_REF);  // to int, to bool, to string: a value word
      }
      break;
    case GLTANG_OP_ADOPT:  // the value, or its copy: a value word (a raw one was the box exit)
      set_stack_kind(c, (uint32_t)d - 1u, KIND_REF);
      break;
    case GLTANG_OP_ARRAY:  // the elements are replaced by the array
      set_stack_kind(c, (uint32_t)(d - (int32_t)a), KIND_REF);
      break;
    case GLTANG_OP_MAP:    // the pairs are replaced by the map
      set_stack_kind(c, (uint32_t)(d - 2 * (int32_t)a), KIND_REF);
      break;
    case GLTANG_OP_ADD:
    case GLTANG_OP_SUB:
    case GLTANG_OP_MUL:
    case GLTANG_OP_DIV:
      // A float result stays raw: either operand being raw makes the operation a float one.
      set_stack_kind(c, (uint32_t)d - 2u, c->sk[d - 2] == KIND_F64 || c->sk[d - 1] == KIND_F64 ? KIND_F64 : KIND_REF);
      break;
    case GLTANG_OP_MOD:
    case GLTANG_OP_LT:
    case GLTANG_OP_LE:
    case GLTANG_OP_GT:
    case GLTANG_OP_GE:
    case GLTANG_OP_EQ:
    case GLTANG_OP_NE:
      set_stack_kind(c, (uint32_t)d - 2u, KIND_REF);
      break;
    case GLTANG_OP_CALL:
      set_stack_kind(c, (uint32_t)d - 1u - a, KIND_REF);
      break;
    case GLTANG_OP_INDEX:  // the container is replaced by the element, a value word
      set_stack_kind(c, (uint32_t)d - 2u, KIND_REF);
      break;
    case GLTANG_OP_SLICE:  // the container is replaced by the slice, a value word
      set_stack_kind(c, (uint32_t)d - 1u - ((a & 1u) + ((a >> 1) & 1u) + ((a >> 2) & 1u)), KIND_REF);
      break;
    // Instructions with no effect on a kind: they pop, jump or leave.
    case GLTANG_OP_POLL:
    case GLTANG_OP_LINE:
    case GLTANG_OP_POP:
    case GLTANG_OP_JMP:
    case GLTANG_OP_JMP_FALSE:
    case GLTANG_OP_JMP_TRUE:
    case GLTANG_OP_AND:
    case GLTANG_OP_OR:
    case GLTANG_OP_RET:
    case GLTANG_OP_PRINT_CONST:
    case GLTANG_OP_CLEAR_RESULT:
    case GLTANG_OP_SET_RESULT:   // pops a value word
    case GLTANG_OP_DISCARD:      // the same
    case GLTANG_OP_STORE_GLOBAL: // the value stays, a value word, as it was
    case GLTANG_OP_HALT:
      break;
    default:
      // An instruction that flows on with a stack effect this function does not know
      // is a defect of the compiler, not a kind to guess.
      return false;
  }
  return true;
}

static void kinds_all_ref(Ctx * c) {
  memset(c->lk, KIND_REF, sizeof(c->lk));
  memset(c->sk, KIND_REF, sizeof(c->sk));
}

// ---------------------------------------------------------------------------
// The declaration scan (which function a global or a local names)
// ---------------------------------------------------------------------------

static void note_declaration(uint32_t * table, uint32_t count, uint32_t slot, uint32_t function) {
  if (slot >= count) {
    return;
  }
  table[slot] = table[slot] == DECL_NONE ? function : DECL_MANY;
}

/**
 * Finds the declarations `compile_function_declaration` emits, `FUNC k; STORE_x s;
 * POP`: for a global in function 0 of the program, for a local in the function
 * itself. A slot with one declaration names that function; a slot with none, or
 * with two, names nothing. Whether the slot still holds that function when a call
 * runs is the call site's guard.
 */
static void scan_declarations(Ctx * c) {
  const GLTANG_Function * top = &c->program->functions[0];
  for (uint32_t j = 0; j + 2u < top->code_count; ++j) {
    uint32_t w = top->code[j];
    if (GLTANG_INSTRUCTION_OP(w) == GLTANG_OP_ITER_NEXT) {
      ++j;  // the second word is a jump target, not an instruction
      continue;
    }
    if (GLTANG_INSTRUCTION_OP(w) == GLTANG_OP_FUNC && GLTANG_INSTRUCTION_OP(top->code[j + 1u]) == GLTANG_OP_STORE_GLOBAL
        && GLTANG_INSTRUCTION_OP(top->code[j + 2u]) == GLTANG_OP_POP) {
      note_declaration(c->global_fn, c->program->global_count, GLTANG_INSTRUCTION_A(top->code[j + 1u]), GLTANG_INSTRUCTION_A(w));
    }
  }
  for (uint32_t j = 0; j + 2u < c->fn->code_count; ++j) {
    uint32_t w = c->fn->code[j];
    if (GLTANG_INSTRUCTION_OP(w) == GLTANG_OP_ITER_NEXT) {
      ++j;
      continue;
    }
    if (GLTANG_INSTRUCTION_OP(w) == GLTANG_OP_FUNC && GLTANG_INSTRUCTION_OP(c->fn->code[j + 1u]) == GLTANG_OP_STORE_LOCAL
        && GLTANG_INSTRUCTION_OP(c->fn->code[j + 2u]) == GLTANG_OP_POP) {
      note_declaration(c->local_fn, c->lc, GLTANG_INSTRUCTION_A(c->fn->code[j + 1u]), GLTANG_INSTRUCTION_A(w));
    }
  }
}

/**
 * The same for `use`: `USE k; STORE_x s; POP` is what `compile_use` emits, and the
 * slot then holds whatever the path named when the statement last ran. Which
 * member that is can be read from the libraries now; whether the slot still holds
 * it when a call runs is the call site's guard.
 */
static void scan_uses(Ctx * c) {
  for (int pass = 0; pass < 2; ++pass) {
    const GLTANG_Function * f = pass == 0 ? &c->program->functions[0] : c->fn;
    if (pass == 1 && f == &c->program->functions[0]) {
      break;  // the top level is the function: its globals were just read
    }
    for (uint32_t j = 0; j + 2u < f->code_count; ++j) {
      uint32_t w = f->code[j];
      if (GLTANG_INSTRUCTION_OP(w) == GLTANG_OP_ITER_NEXT) {
        ++j;
        continue;
      }
      if (GLTANG_INSTRUCTION_OP(w) != GLTANG_OP_USE || GLTANG_INSTRUCTION_OP(f->code[j + 2u]) != GLTANG_OP_POP) {
        continue;
      }
      GLTANG_Opcode store = GLTANG_INSTRUCTION_OP(f->code[j + 1u]);
      uint32_t slot = GLTANG_INSTRUCTION_A(f->code[j + 1u]);
      if (store == GLTANG_OP_STORE_GLOBAL) {
        note_declaration(c->global_use, c->program->global_count, slot, GLTANG_INSTRUCTION_A(w));
      }
      else if (store == GLTANG_OP_STORE_LOCAL && f == c->fn) {
        note_declaration(c->local_use, c->lc, slot, GLTANG_INSTRUCTION_A(w));
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Which library member a value names (the libraries as they are now)
// ---------------------------------------------------------------------------

/** The string constant at an index: its bytes, or NULL. */
static const char * constant_text(const Ctx * c, uint32_t index, size_t * length) {
  if (index >= c->program->constant_count || c->program->constants[index].kind != GLTANG_CONST_STRING) {
    return NULL;
  }
  const GLTANG_StringBlock * block = c->program->constants[index].block;
  *length = (size_t)block->byte_length;
  return gltang_string_bytes(block);
}

/** The member a `use` path names, walked as `gltang_vm_resolve` walks it (a library for every part but the last), or NULL. */
static const GLTANG_LibraryMember * path_member(const Ctx * c, uint32_t constant) {
  size_t length = 0;
  const char * text = constant_text(c, constant, &length);
  if (!text) {
    return NULL;
  }
  const char * dot = memchr(text, '.', length);
  size_t first = dot ? (size_t)(dot - text) : length;
  const GLTANG_Library * layers[3] = {c->exec->libraries, c->program->libraries, gltang_library_builtins()};
  const GLTANG_LibraryMember * member = NULL;
  for (size_t i = 0; i < 3 && !member; ++i) {
    member = gltang_library_find(layers[i], text, first);
  }
  size_t at = first;
  while (member && at < length) {
    if (member->kind != GLTANG_MEMBER_LIBRARY || !member->library) {
      return NULL;
    }
    ++at;
    size_t next = at;
    while (next < length && text[next] != '.') {
      ++next;
    }
    member = gltang_library_find(member->library, text + at, next - at);
    at = next;
  }
  return member;
}

/** The member the value an instruction produced is, if the declarations and the libraries say so; NULL if not or if not known. */
static const GLTANG_LibraryMember * static_member(const Ctx * c, uint32_t producer, int depth) {
  if (producer == PROD_NONE || producer >= c->fn->code_count || depth > 8) {
    return NULL;
  }
  GLTANG_Opcode op = GLTANG_INSTRUCTION_OP(c->fn->code[producer]);
  uint32_t a = GLTANG_INSTRUCTION_A(c->fn->code[producer]);
  switch (op) {
    case GLTANG_OP_LOAD_GLOBAL:
      if (a < c->program->global_count && c->global_use[a] != DECL_NONE && c->global_use[a] != DECL_MANY && c->global_fn[a] == DECL_NONE) {
        return path_member(c, c->global_use[a]);
      }
      return NULL;
    case GLTANG_OP_LOAD_LOCAL:
      if (a < c->lc && c->local_use[a] != DECL_NONE && c->local_use[a] != DECL_MANY && c->local_fn[a] == DECL_NONE) {
        return path_member(c, c->local_use[a]);
      }
      return NULL;
    case GLTANG_OP_ATTR: {
      const GLTANG_LibraryMember * library = static_member(c, c->attr_src[producer], depth + 1);
      size_t length = 0;
      const char * name = library && library->kind == GLTANG_MEMBER_LIBRARY && library->library ? constant_text(c, a, &length) : NULL;
      return name ? gltang_library_find(library->library, name, length) : NULL;
    }
    default:
      return NULL;
  }
}

/** Whether a member is one a load can produce without an exit: a host function or a library. */
static bool loadable(const GLTANG_LibraryMember * member) {
  return member && (member->kind == GLTANG_MEMBER_NATIVE || member->kind == GLTANG_MEMBER_LIBRARY);
}

static bool use_load_ok(const Ctx * c, uint32_t constant) {
  return loadable(path_member(c, constant));
}

/** The host function a `CALL n` calls, given the instruction that produced its callee, or NULL: a library native, not a resumable one. */
static const GLTANG_LibraryMember * resolve_native(const Ctx * c, uint32_t producer, uint32_t n) {
  if (n > GLTANG_JIT_MAX_CALL_ARGS) {
    return NULL;
  }
  const GLTANG_LibraryMember * member = static_member(c, producer, 0);
  return member && member->kind == GLTANG_MEMBER_NATIVE && !(member->native_flags & GLTANG_NATIVE_FLAG_RESUMABLE) ? member : NULL;
}

/** Marks the instructions a jump lands on. The one two-word instruction is skipped over. */
static void scan_targets(Ctx * c) {
  const uint32_t n = c->fn->code_count;
  for (uint32_t j = 0; j < n; ++j) {
    GLTANG_Opcode op = GLTANG_INSTRUCTION_OP(c->fn->code[j]);
    uint32_t a = GLTANG_INSTRUCTION_A(c->fn->code[j]);
    switch (op) {
      case GLTANG_OP_JMP:
      case GLTANG_OP_JMP_FALSE:
      case GLTANG_OP_JMP_TRUE:
      case GLTANG_OP_AND:
      case GLTANG_OP_OR:
        if (a < n) {
          c->target[a] = true;
        }
        break;
      case GLTANG_OP_ITER_NEXT:
        ++j;
        break;
      default:
        break;
    }
  }
  c->target[1] = true;
}

/**
 * The function a `CALL n` calls, given the instruction that produced its callee
 * operand, or -1: (R1) that instruction is one `LOAD_GLOBAL` or `LOAD_LOCAL`; (R2)
 * the declaration scan names the slot's function k; (R3) k takes n parameters,
 * n plus the hidden flag fit the call's argument limit, and k's frame fits.
 */
static int32_t resolve_callee(const Ctx * c, uint32_t producer, uint32_t n) {
  if (!c->calls_on || producer == PROD_NONE || n > GLTANG_JIT_MAX_CALL_ARGS) {
    return -1;
  }
  GLTANG_Opcode op = GLTANG_INSTRUCTION_OP(c->fn->code[producer]);
  uint32_t slot = GLTANG_INSTRUCTION_A(c->fn->code[producer]);
  uint32_t k = DECL_NONE;
  if (op == GLTANG_OP_LOAD_GLOBAL && slot < c->program->global_count) {
    k = c->global_fn[slot];
  }
  else if (op == GLTANG_OP_LOAD_LOCAL && slot < c->lc) {
    k = c->local_fn[slot];
  }
  if (k == DECL_NONE || k == DECL_MANY || k == 0 || k >= c->program->function_count) {
    return -1;
  }
  const GLTANG_Function * callee = &c->program->functions[k];
  if (callee->parameter_count != n || callee->frame_slots > GLTANG_JIT_MAX_SLOTS || callee->code_count < 2u) {
    return -1;
  }
  return (int32_t)k;
}

// ---------------------------------------------------------------------------
// The stack-depth and producer analysis over the code reachable through compiled paths
// ---------------------------------------------------------------------------

/**
 * A way into `target` with `d` operands, whose positions were produced by `cur`.
 * The first way in records them; every later one merges, and a position whose
 * producers differ is left unnamed. A change sends the target to the worklist
 * again, so what flows from it is walked with the merged producers.
 */
static bool reach(Ctx * c, uint32_t target, int32_t d, const uint32_t * cur, size_t * wn) {
  if (target >= c->fn->code_count) {
    return false;
  }
  if (c->depth[target] < 0) {
    uint32_t * mine = gcu_allocator_malloc(GLTANG_JIT_ALLOCATOR(c->exec), ((size_t)c->ms + 1u) * sizeof(uint32_t));
    if (!mine) {
      c->oom = true;
      return false;
    }
    if (d > 0) {
      memcpy(mine, cur, (size_t)d * sizeof(uint32_t));
    }
    c->prod_at[target] = mine;
    uint8_t * kinds = gcu_allocator_malloc(GLTANG_JIT_ALLOCATOR(c->exec), (size_t)c->lc + (size_t)c->ms + 1u);
    if (!kinds) {
      c->oom = true;
      return false;
    }
    memcpy(kinds, c->lk, c->lc);
    if (d > 0) {
      memcpy(kinds + c->lc, c->sk, (size_t)d);
    }
    c->kind_at[target] = kinds;
    c->depth[target] = d;
    c->queued[target] = true;
    c->work[(*wn)++] = target;
    return true;
  }
  if (c->depth[target] != d) {
    return false;
  }
  // The kinds must agree: a slot raw on one way in and a reference on another has
  // no register to name. That is not merged, it is a conflict, and the function is
  // then compiled with no raw source at all (see analyse).
  if (memcmp(c->kind_at[target], c->lk, c->lc) != 0 || (d > 0 && memcmp(c->kind_at[target] + c->lc, c->sk, (size_t)d) != 0)) {
    c->conflict = true;
  }
  bool changed = false;
  uint32_t * mine = c->prod_at[target];
  for (int32_t p = 0; p < d; ++p) {
    if (mine[p] != cur[p] && mine[p] != PROD_NONE) {
      mine[p] = PROD_NONE;
      changed = true;
    }
  }
  if (changed && !c->queued[target]) {
    c->queued[target] = true;
    c->work[(*wn)++] = target;
  }
  return true;
}

static GLTANG_Result analyse_once(Ctx * c) {
  const uint32_t * code = c->fn->code;
  size_t wn = 0;
  c->leader[1] = true;
  kinds_all_ref(c);  // a parameter is a reference and every other local starts as the zero value
  if (!reach(c, 1, 0, c->cur, &wn)) {
    return c->oom ? GLTANG_ERR_OOM : GLTANG_ERR_INTERNAL;
  }
  while (wn) {
    uint32_t i = c->work[--wn];
    c->queued[i] = false;
    if (c->depth[i] > 0) {
      memcpy(c->cur, c->prod_at[i], (size_t)c->depth[i] * sizeof(uint32_t));
    }
    memcpy(c->lk, c->kind_at[i], c->lc);
    if (c->depth[i] > 0) {
      memcpy(c->sk, c->kind_at[i] + c->lc, (size_t)c->depth[i]);
    }
    for (;;) {
      GLTANG_Opcode op = GLTANG_INSTRUCTION_OP(code[i]);
      uint32_t a = GLTANG_INSTRUCTION_A(code[i]);
      int32_t d = c->depth[i];
      int32_t after = d;
      bool falls = true;
      bool branch = false;       // a conditional or unconditional jump to `a`
      int32_t branch_depth = d;
      c->end_kind[i] = END_NONE;  // this walk decides how the instruction ends: a walk of it with other kinds is not remembered
      if (op == GLTANG_OP_ADD && d >= 2) {
        // Which side is a string constant, as this walk's producers say (the last walk's is
        // what the emission reads, as for every other decision of the walk).
        c->str_side[i] = (uint8_t)((string_producer(c, c->cur[d - 2]) ? 1u : 0u) | (string_producer(c, c->cur[d - 1]) ? 2u : 0u));
      }
      if (op == GLTANG_OP_CAST && a == GLTANG_CAST_TYPE_INTEGER && d >= 1) {
        // Whether the operand is a float constant, raw as it stands (this walk's producer and
        // kind; the last walk's is what the emission reads): the fold or the exit of constant_cast.
        const uint32_t p = c->cur[d - 1];
        const bool constant = c->sk[d - 1] == KIND_F64 && p != PROD_NONE && p < c->fn->code_count
            && GLTANG_INSTRUCTION_OP(c->fn->code[p]) == GLTANG_OP_CONST;
        c->cast_src[i] = constant ? p : PROD_NONE;
      }
      if (reads_raw(c, i, op, a, d)) {
        c->end_kind[i] = END_EXPECTED;  // designed: the float is boxed for its consumer (see emit_op_kinds)
        break;                   // an unconditional exit with the raw operand in its frame state: nothing flows from it
      }
      if (op == GLTANG_OP_CALL) {
        if (d < 0 || (int64_t)a + 1 > d) { return GLTANG_ERR_INTERNAL; }
        int32_t k = c->force_exit[i] ? -1 : resolve_callee(c, c->cur[d - 1 - (int32_t)a], a);
        const GLTANG_LibraryMember * native = NULL;
        if (k < 0 && !c->force_exit[i]) {
          // Not a guest function: a library native, if the declarations name one.
          native = resolve_native(c, c->cur[d - 1 - (int32_t)a], a);
          if (native && !c->natives_on) {
            c->declined[i] = true;
            native = NULL;
          }
        }
        c->call_k[i] = k;
        c->call_member[i] = native;
        if (k < 0 && !native) {
          c->end_kind[i] = END_REAL;
          break;                 // an unconditional exit: nothing flows from it
        }
        c->flowed[i] = true;
        after = d - (int32_t)a;
        c->cur[after - 1] = i;
      }
      else if (op == GLTANG_OP_ATTR) {
        // `.name` of a library: a member load when the declarations say the operand
        // is one and the library has the member.
        if (d < 1) { return GLTANG_ERR_INTERNAL; }
        c->attr_src[i] = c->cur[d - 1];
        const GLTANG_LibraryMember * library = static_member(c, c->cur[d - 1], 0);
        size_t length = 0;
        const char * name = library && library->kind == GLTANG_MEMBER_LIBRARY && library->library ? constant_text(c, a, &length) : NULL;
        bool ok = name && loadable(gltang_library_find(library->library, name, length)) && !c->force_exit[i];
        c->load_ok[i] = ok && c->natives_on;
        if (ok && !c->natives_on) {
          c->declined[i] = true;
        }
        // Any other value's attribute is the interpreter's attribute rule called from compiled
        // code (story 3), behind a test that the value is not a host-defined one: so where
        // natives can be called every ATTR flows on, one way or the other, and a walk that
        // later finds the operand is not the library it thought only changes which call it is.
        if (!c->natives_on) {
          c->end_kind[i] = END_REAL;
          break;                 // an unconditional exit: nothing flows from it
        }
        c->cur[d - 1] = i;
      }
      else if (op == GLTANG_OP_CAST && a == GLTANG_CAST_TYPE_INTEGER && constant_cast(c, i, &(int64_t){0}) == CAST_FOLD_OUT) {
        c->end_kind[i] = END_REAL;  // not END_EXPECTED: the static decline must not take it (see emit_op_kinds)
        break;                   // an unconditional, designed exit: the interpreter makes the boxed integer or the error
      }
      else if (!inline_op(c, op, a)) {
        if (op == GLTANG_OP_USE && !c->natives_on && use_load_ok(c, a)) {
          c->declined[i] = true;
        }
        c->end_kind[i] = END_REAL;
        break;                   // an unconditional exit: nothing flows from it
      }
      else switch (op) {
        case GLTANG_OP_POLL:
        case GLTANG_OP_LINE:
        case GLTANG_OP_PRINT_CONST:
        case GLTANG_OP_CLEAR_RESULT:
          break;
        case GLTANG_OP_PRINT:
          // The value replaces the operand: depth unchanged.
          if (d < 1) { return GLTANG_ERR_INTERNAL; }
          c->cur[d - 1] = i;
          break;
        case GLTANG_OP_POP:
          if (d < 1) { return GLTANG_ERR_INTERNAL; }
          after = d - 1;
          break;
        case GLTANG_OP_DUP:
          if (d < 1) { return GLTANG_ERR_INTERNAL; }
          c->cur[d] = i;
          after = d + 1;
          break;
        case GLTANG_OP_NULL:
        case GLTANG_OP_TRUE:
        case GLTANG_OP_FALSE:
        case GLTANG_OP_CONST:
        case GLTANG_OP_LOAD_GLOBAL:
        case GLTANG_OP_FUNC:
        case GLTANG_OP_USE:
          c->cur[d] = i;
          after = d + 1;
          break;
        case GLTANG_OP_LOAD_LOCAL:
          if (a >= c->lc) { return GLTANG_ERR_INTERNAL; }
          c->cur[d] = i;
          after = d + 1;
          break;
        case GLTANG_OP_STORE_LOCAL:
          if (a >= c->lc || d < 1) { return GLTANG_ERR_INTERNAL; }
          break;
        case GLTANG_OP_STORE_GLOBAL:
          // The value stays on the stack, with its producer.
          if (d < 1) { return GLTANG_ERR_INTERNAL; }
          break;
        case GLTANG_OP_SET_RESULT:
        case GLTANG_OP_DISCARD:
          if (d < 1) { return GLTANG_ERR_INTERNAL; }
          after = d - 1;
          break;
        case GLTANG_OP_HALT:
          // A return (of the halted word): the end of a path, as RET is.
          c->end_kind[i] = END_RETURN;
          falls = false;
          break;
        case GLTANG_OP_NEG:
        case GLTANG_OP_NOT:
        case GLTANG_OP_CAST:
        case GLTANG_OP_ADOPT:
          if (d < 1) { return GLTANG_ERR_INTERNAL; }
          c->cur[d - 1] = i;
          break;
        case GLTANG_OP_ARRAY:
        case GLTANG_OP_MAP: {
          // The container replaces its elements (or its pairs): one value.
          const int64_t n = op == GLTANG_OP_MAP ? 2 * (int64_t)a : (int64_t)a;
          if ((int64_t)d < n) { return GLTANG_ERR_INTERNAL; }
          after = (int32_t)((int64_t)d - n) + 1;
          c->cur[after - 1] = i;
          break;
        }
        case GLTANG_OP_INDEX:
          // The element replaces the container and the index: one fewer.
          if (d < 2) { return GLTANG_ERR_INTERNAL; }
          c->cur[d - 2] = i;
          after = d - 1;
          break;
        case GLTANG_OP_SLICE: {
          // The slice replaces the container and its parts (one per flag bit).
          const int32_t parts = (int32_t)((a & 1u) + ((a >> 1) & 1u) + ((a >> 2) & 1u));
          if (d < parts + 1) { return GLTANG_ERR_INTERNAL; }
          c->cur[d - 1 - parts] = i;
          after = d - parts;
          break;
        }
        case GLTANG_OP_ADD:
        case GLTANG_OP_SUB:
        case GLTANG_OP_MUL:
        case GLTANG_OP_DIV:
        case GLTANG_OP_MOD:
        case GLTANG_OP_LT:
        case GLTANG_OP_LE:
        case GLTANG_OP_GT:
        case GLTANG_OP_GE:
        case GLTANG_OP_EQ:
        case GLTANG_OP_NE:
          if (d < 2) { return GLTANG_ERR_INTERNAL; }
          c->cur[d - 2] = i;
          after = d - 1;
          break;
        case GLTANG_OP_JMP:
          falls = false;
          branch = true;
          break;
        case GLTANG_OP_JMP_FALSE:
        case GLTANG_OP_JMP_TRUE:
          if (d < 1) { return GLTANG_ERR_INTERNAL; }
          after = d - 1;
          branch = true;
          branch_depth = d - 1;
          break;
        case GLTANG_OP_AND:
        case GLTANG_OP_OR:
          // The value stays for the jump and is popped for the fall-through.
          if (d < 1) { return GLTANG_ERR_INTERNAL; }
          after = d - 1;
          branch = true;
          branch_depth = d;
          break;
        case GLTANG_OP_RET:
          if (d < 1) { return GLTANG_ERR_INTERNAL; }
          c->end_kind[i] = END_RETURN;
          falls = false;
          break;
        default:
          return GLTANG_ERR_INTERNAL;
      }
      if (after > (int32_t)c->ms) {
        return GLTANG_ERR_INTERNAL;
      }
      if (!apply_kinds(c, op, a, d)) {
        return GLTANG_ERR_INTERNAL;
      }
      if (branch) {
        if (a >= c->fn->code_count) { return GLTANG_ERR_INTERNAL; }
        c->leader[a] = true;
        c->back_branch = c->back_branch || a <= i;
        if (!reach(c, a, branch_depth, c->cur, &wn)) { return c->oom ? GLTANG_ERR_OOM : GLTANG_ERR_INTERNAL; }
      }
      if (!falls) {
        break;
      }
      uint32_t next = i + 1u;
      if (next >= c->fn->code_count) {
        return GLTANG_ERR_INTERNAL;
      }
      if (branch) {
        c->leader[next] = true;      // a conditional branch ends its block
      }
      if (c->target[next]) {
        // A jump lands here: every way in merges, through the worklist.
        c->leader[next] = true;
        if (!reach(c, next, after, c->cur, &wn)) { return c->oom ? GLTANG_ERR_OOM : GLTANG_ERR_INTERNAL; }
        break;
      }
      if (c->depth[next] >= 0 && c->depth[next] != after) { return GLTANG_ERR_INTERNAL; }
      c->depth[next] = after;
      i = next;
    }
  }
  return GLTANG_OK;
}

static void free_prod(Ctx * c) {
  if (c->prod_at) {
    for (size_t i = 0; i < c->fn->code_count; ++i) {
      gcu_allocator_free(GLTANG_JIT_ALLOCATOR(c->exec), c->prod_at[i]);
      c->prod_at[i] = NULL;
      if (c->kind_at) {
        gcu_allocator_free(GLTANG_JIT_ALLOCATOR(c->exec), c->kind_at[i]);
        c->kind_at[i] = NULL;
      }
    }
  }
}

/**
 * The analysis, repeated until no call that the walk went on past is unnamable
 * at the end of it: a merge can leave a callee operand with two producers after
 * the walk had already treated the call as a call and gone on to what follows,
 * and that call is then an exit, which makes what follows it unreachable.
 */
static GLTANG_Result analyse(Ctx * c) {
  for (;;) {
    c->conflict = false;
    memset(c->ever_local, 0, sizeof(c->ever_local));
    memset(c->ever_stack, 0, sizeof(c->ever_stack));
    for (size_t i = 0; i < c->fn->code_count; ++i) {
      c->depth[i] = -1;
      c->leader[i] = false;
      c->queued[i] = false;
      c->flowed[i] = false;
      c->call_k[i] = -1;
      c->call_member[i] = NULL;
      c->load_ok[i] = false;
      c->declined[i] = false;
      c->attr_src[i] = PROD_NONE;
      c->cast_src[i] = PROD_NONE;
      c->end_kind[i] = END_NONE;
    }
    c->back_branch = false;
    free_prod(c);
    GLTANG_Result r = analyse_once(c);
    if (r != GLTANG_OK) {
      return r;
    }
    bool again = false;
    if (c->conflict) {
      if (c->raw_off) {
        return GLTANG_ERR_INTERNAL;  // with no raw source the kinds cannot disagree: looping would never end
      }
      // The blunt rule: every raw source of the function becomes an exit and the
      // analysis is repeated. With no raw source the kinds cannot disagree, so this
      // happens at most once; the function is then compiled as it was before.
      c->raw_off = true;
      continue;
    }
    // Only a CALL can need it: an ATTR flows on whatever its operand turns out to be (a
    // library's member or any other value), so only a CALL is marked as having flowed.
    for (size_t i = 0; i < c->fn->code_count; ++i) {
      if (!c->flowed[i]) {
        continue;
      }
      if (c->call_k[i] < 0 && !c->call_member[i]) {
        c->force_exit[i] = true;
        again = true;
      }
    }
    if (!again) {
      return GLTANG_OK;
    }
  }
}

// ---------------------------------------------------------------------------
// Emission
// ---------------------------------------------------------------------------

static GRCORE_PollIdentity identity_at(const Ctx * c, uint32_t index) {
  return (GRCORE_PollIdentity){c->fword, index, c->exec->engine};
}

/**
 * The frame state: the interpreter's frame as it is when the instruction at `pc`
 * is about to run (or has just run, for a poll), in the guest frame's own slot
 * order and exactly `frame_slots` long. The three header words are constants and
 * the flags a register. A local is its register. An operand-stack position below
 * the depth is its register and above it is the zero register, which reads as the
 * zero value slot of the interpreter's frame (a `DEAD` location reads as a raw
 * zero, which is not what the frame holds). A slot that is raw at this point (its
 * kind in `lk` and `sk`) names its `F64` register, and the other is not named, so
 * not live, so not in a stack map: the backend records a raw location of
 * representation `F64`, which a rebuild boxes.
 */
static void fill_state(Ctx * c, GRJIT_FrameSlot * s, uint32_t pc, uint32_t depth) {
  s[GLTANG_F_FUNCTION] = grjit_frame_slot_constant((int64_t)c->fword);
  s[GLTANG_F_PC] = grjit_frame_slot_constant((int64_t)pc);
  s[GLTANG_F_SP] = grjit_frame_slot_constant((int64_t)(H + c->lc + depth));
  s[GLTANG_F_FLAGS] = grjit_frame_slot_vreg(c->flags);
  for (uint32_t k = 0; k < c->lc; ++k) {
    s[H + k] = grjit_frame_slot_vreg(c->lk[k] == KIND_F64 ? c->local_f[k] : c->local[k]);
  }
  for (uint32_t j = 0; j < c->ms; ++j) {
    s[H + c->lc + j] = grjit_frame_slot_vreg(j >= depth ? c->zero : c->sk[j] == KIND_F64 ? c->stack_f[j] : c->stack[j]);
  }
}

static void bin(Ctx * c, GRJIT_OpKind kind, GRJIT_VReg dst, GRJIT_Operand a, GRJIT_Operand b) {
  B(grjit_builder_binary(c->b, kind, dst, a, b));
}

static void cmp(Ctx * c, GRJIT_Cmp cond, GRJIT_VReg dst, GRJIT_Operand a, GRJIT_Operand b) {
  B(grjit_builder_cmp(c->b, cond, dst, a, b));
}

static void bitcast(Ctx * c, GRJIT_VReg dst, GRJIT_VReg src) {
  B(grjit_builder_bitcast(c->b, dst, src));
}

/** Counts an operation's cost; the add to memory is made by `flush_cost`. */
static void add_fuel(Ctx * c, uint32_t cost) {
  c->pending_cost += cost;
}

/**
 * Adds the fuel counted since the last add to the execution's `pending_fuel`.
 * Called before everything that can observe the fuel or leave: a guard, a call, a
 * poll, a branch and `RET`. The fuel is in memory and in no register or frame
 * state, so an exit and a chain deoptimization lose nothing.
 */
static void flush_cost(Ctx * c) {
  if (!c->pending_cost) {
    return;
  }
  GRJIT_VReg f = c->tmp[TMP_F];
  B(grjit_builder_load(c->b, f, c->ep, (int32_t)offsetof(GLTANG_Execution, pending_fuel), 64, false));
  bin(c, GRJIT_OP_ADD, f, V(f), I((int64_t)c->pending_cost));
  B(grjit_builder_store(c->b, c->ep, (int32_t)offsetof(GLTANG_Execution, pending_fuel), 64, V(f)));
  c->pending_cost = 0;
}

static void new_block(Ctx * c, GRJIT_BlockId * out);
static void set_block(Ctx * c, GRJIT_BlockId id);

static void guard(Ctx * c, GRJIT_Operand cond, uint32_t index, uint32_t depth) {
  flush_cost(c);
  if (c->guard_to_on) {
    // Inside the small-integer path of `+`: a failed test goes to the slow path, the
    // interpreter's `+` called from compiled code, with nothing of the operation done.
    GRJIT_BlockId go_on;
    new_block(c, &go_on);
    B(grjit_builder_br_if(c->b, cond, go_on, c->guard_to));
    c->open = false;
    set_block(c, go_on);
    return;
  }
  fill_state(c, c->state, index, depth);
  B(grjit_builder_guard(c->b, cond, identity_at(c, index), c->state, c->slots));
}

/** A call to a helper that is not a GC point: `helper(exec)`. */
static void call_exec_helper(Ctx * c, uint64_t helper) {
  GRJIT_Operand args[1] = {I((int64_t)(intptr_t)c->exec)};
  B(grjit_builder_call(c->b, GRJIT_NO_VREG, helper, GRJIT_CALL_NO_GC,
      GRCORE_SITE_GC_POINT_CALL, args, 1, (GRCORE_PollIdentity){0, 0, 0},
      NULL, 0));
}

/** The charge, the flush and the poll of the interpreter's POLL (and LINE). */
static void emit_poll(Ctx * c, uint32_t index, uint32_t depth) {
  flush_cost(c);
  call_exec_helper(c, (uint64_t)(uintptr_t)gltang_jit_flush);
  fill_state(c, c->state, index + 1u, depth);
  B(grjit_builder_poll(c->b, identity_at(c, index), c->state, c->slots));
}

/** `dst = ((raw & 15) == tag)`, for a raw word. */
static void tag_is(Ctx * c, GRJIT_VReg dst, GRJIT_VReg raw, int64_t tag) {
  bin(c, GRJIT_OP_AND, c->tmp[TMP_U], V(raw), I(0x0F));
  cmp(c, GRJIT_CMP_EQ, dst, V(c->tmp[TMP_U]), I(tag));
}

/** Guards that both operands of a binary operation at `depth` are small integers, and leaves them as raw words. */
static void guard_two_ints(Ctx * c, uint32_t index, uint32_t depth) {
  GRJIT_VReg a = c->tmp[TMP_A], b = c->tmp[TMP_B];
  bitcast(c, a, c->stack[depth - 2u]);
  bitcast(c, b, c->stack[depth - 1u]);
  tag_is(c, c->tmp[TMP_C], a, GLTANG_TAG_INTEGER);
  tag_is(c, c->tmp[TMP_D], b, GLTANG_TAG_INTEGER);
  bin(c, GRJIT_OP_AND, c->tmp[TMP_C], V(c->tmp[TMP_C]), V(c->tmp[TMP_D]));
  guard(c, V(c->tmp[TMP_C]), index, depth);
}

/** Guards that a result `r` is in the small-integer range. */
static void guard_in_range(Ctx * c, GRJIT_VReg r, uint32_t index, uint32_t depth) {
  bin(c, GRJIT_OP_SUB, c->tmp[TMP_U], V(r), I(GLTANG_SMALL_INT_MIN));
  cmp(c, GRJIT_CMP_ULE, c->tmp[TMP_C], V(c->tmp[TMP_U]), I((int64_t)((uint64_t)GLTANG_SMALL_INT_MAX - (uint64_t)GLTANG_SMALL_INT_MIN)));
  guard(c, V(c->tmp[TMP_C]), index, depth);
}

/** `dst (a reference) = (r << 4) | tag`. */
static void box(Ctx * c, GRJIT_VReg dst, GRJIT_VReg r, int64_t tag) {
  bin(c, GRJIT_OP_SHL, c->tmp[TMP_U], V(r), I(4));
  bin(c, GRJIT_OP_OR, c->tmp[TMP_U], V(c->tmp[TMP_U]), I(tag));
  bitcast(c, dst, c->tmp[TMP_U]);
}

static void new_block(Ctx * c, GRJIT_BlockId * out) {
  B(grjit_builder_block(c->b, out));
}

static void set_block(Ctx * c, GRJIT_BlockId id) {
  B(grjit_builder_set_block(c->b, id));
  c->open = true;
}

static void branch_to(Ctx * c, GRJIT_BlockId id) {
  flush_cost(c);
  B(grjit_builder_br(c->b, id));
  c->open = false;
}

/** A double as an `F64` immediate operand (its bit pattern). */
static GRJIT_Operand FI(double d) {
  uint64_t u;
  memcpy(&u, &d, sizeof(u));
  return grjit_operand_f64_imm(u);
}

/**
 * The float object a value word names, read as a double into `dst`, in the current
 * block: the word must be a non-null pointer (tag 0) to an object whose kind is
 * `GLTANG_OBJ_FLOAT`, each a guard that exits with the frame as it is (nothing has
 * been written), and only then is the object read. The load of `kind` is reached only
 * after the pointer test. No GC point lies between the test and the load.
 */
static void unbox_float_object(Ctx * c, uint32_t i, uint32_t d, uint32_t pos, GRJIT_VReg dst) {
  GRJIT_VReg * t = c->tmp;
  bitcast(c, t[TMP_A], c->stack[pos]);
  tag_is(c, t[TMP_C], t[TMP_A], 0);
  cmp(c, GRJIT_CMP_NE, t[TMP_D], V(t[TMP_A]), I(0));
  bin(c, GRJIT_OP_AND, t[TMP_C], V(t[TMP_C]), V(t[TMP_D]));
  guard(c, V(t[TMP_C]), i, d);
  B(grjit_builder_load(c->b, t[TMP_X], c->stack[pos], (int32_t)offsetof(GLTANG_FloatObject, kind), 32, false));
  cmp(c, GRJIT_CMP_EQ, t[TMP_C], V(t[TMP_X]), I((int64_t)GLTANG_OBJ_FLOAT));
  guard(c, V(t[TMP_C]), i, d);
  B(grjit_builder_load(c->b, dst, c->stack[pos], (int32_t)offsetof(GLTANG_FloatObject, value), 64, false));
}

/**
 * The operand at stack position `pos` (a `REF`) read as a double into `dst` (an `F64`
 * temporary): a small integer is converted (`SAR 4`, then `CONVERT_S`, which is
 * `(double)n`), a float object is read, and anything else (null, a boolean, a string,
 * a boxed integer, a container, an error) is a guard exit before anything is written.
 * A read, once per use: nothing is cached, stored back or named in a frame state.
 */
static void unbox(Ctx * c, uint32_t i, uint32_t d, uint32_t pos, GRJIT_VReg dst) {
  GRJIT_VReg * t = c->tmp;
  GRJIT_BlockId small, other, join;
  new_block(c, &small);
  new_block(c, &other);
  new_block(c, &join);
  bitcast(c, t[TMP_A], c->stack[pos]);
  tag_is(c, t[TMP_C], t[TMP_A], GLTANG_TAG_INTEGER);
  flush_cost(c);  // before the branch, so that neither arm holds a charge the other does not
  B(grjit_builder_br_if(c->b, V(t[TMP_C]), small, other));
  c->open = false;
  set_block(c, small);
  bin(c, GRJIT_OP_SAR, t[TMP_X], V(t[TMP_A]), I(4));
  B(grjit_builder_unary(c->b, GRJIT_OP_CONVERT_S, dst, V(t[TMP_X])));
  branch_to(c, join);
  set_block(c, other);
  unbox_float_object(c, i, d, pos, dst);
  branch_to(c, join);
  set_block(c, join);
}

/** The double of an operand: a raw one is its register, a `REF` one is unboxed into `scratch`. */
static GRJIT_VReg float_operand(Ctx * c, uint32_t i, uint32_t d, uint32_t pos, GRJIT_VReg scratch) {
  if (c->sk[pos] == KIND_F64) {
    return c->stack_f[pos];
  }
  unbox(c, i, d, pos, scratch);
  return scratch;
}

/**
 * `r = (int64)f`, for a double inside the small-integer range: the exact bounds `f >= -2^59`
 * and `f < 2^59` as guards (a NaN fails both ordered comparisons), then `TRUNC_S`, all in
 * one block as the verifier requires. The guards come first: a double outside them is the
 * interpreter's boxed integer or its error.
 */
static void trunc_small(Ctx * c, uint32_t i, uint32_t d, GRJIT_VReg f, GRJIT_VReg r) {
  GRJIT_VReg * t = c->tmp;
  cmp(c, GRJIT_CMP_GE, t[TMP_C], V(f), FI(-0x1p59));
  guard(c, V(t[TMP_C]), i, d);
  cmp(c, GRJIT_CMP_LT, t[TMP_C], V(f), FI(0x1p59));
  guard(c, V(t[TMP_C]), i, d);
  B(grjit_builder_unary(c->b, GRJIT_OP_TRUNC_S, r, V(f)));
}

/** Guards that the divisor `y` (an untagged integer temporary) is not zero. */
static void guard_nonzero(Ctx * c, GRJIT_VReg y, uint32_t index, uint32_t depth) {
  cmp(c, GRJIT_CMP_NE, c->tmp[TMP_C], V(y), I(0));
  guard(c, V(c->tmp[TMP_C]), index, depth);
}

/** An unconditional deoptimization exit at `index`: the interpreter runs it. */
static void emit_exit(Ctx * c, uint32_t index, uint32_t depth) {
  guard(c, I(0), index, depth);
  B(grjit_builder_ret(c->b, grjit_operand_none()));
  c->open = false;
}

/**
 * A `CALL n` of function `k` at `i`, with `d` operands: the callee value is
 * guarded equal to the function value the site names (a mismatch is a guard
 * exit, counted against this function), and the call goes through `k`'s entry
 * slot with the callee's token and the `n` operands plus the hidden flag, 0.
 *
 * The call carries two frame states, both this function's guest frame: while the
 * callee runs (the caller at the instruction after the `CALL`, with the callee and
 * its arguments popped and the result still to push) and before the call, for an
 * exit (`pc` at the `CALL`, which the interpreter then makes). Both sites have the
 * identity the interpreter's `grcore_stack_set_identity` records at a call.
 */
static void emit_call(Ctx * c, uint32_t i, uint32_t d, uint32_t n, uint32_t k) {
  GRJIT_VReg * T = c->stack;
  GRJIT_VReg * t = c->tmp;
  const uint32_t at = d - 1u - n;
  bitcast(c, t[TMP_A], T[at]);
  cmp(c, GRJIT_CMP_EQ, t[TMP_C], V(t[TMP_A]), I((int64_t)gltang_v_from_function(k)));
  GRJIT_BlockId good, bad;
  new_block(c, &good);
  new_block(c, &bad);
  flush_cost(c);
  B(grjit_builder_br_if(c->b, V(t[TMP_C]), good, bad));
  c->open = false;
  set_block(c, bad);
  call_exec_helper(c, (uint64_t)(uintptr_t)gltang_jit_note_callee_guard);
  emit_exit(c, i, d);
  set_block(c, good);

  GRJIT_Operand args[GLTANG_JIT_MAX_CALL_ARGS + 1u];
  for (uint32_t j = 0; j < n; ++j) {
    args[j] = V(T[at + 1u + j]);
  }
  args[n] = I(0);
  fill_state(c, c->state2, i + 1u, at);
  fill_state(c, c->state, i, d);
  // The callee's entry slot is found through the environment, so the code holds no
  // execution's address (it is shareable): the execution's JIT state, its table of
  // the program's functions and that function's record, whose first word is the
  // slot. Three loads and no call. Every function of the program has its slot
  // before the program's first function is compiled (`gltang_jit_prepare_slots`),
  // so the last load is never null.
  B(grjit_builder_load(c->b, c->callee_tbl, c->jitp, (int32_t)offsetof(GLTANG_Jit, programs), 64, false));
  B(grjit_builder_load(c->b, c->callee_tbl, c->callee_tbl,
      (int32_t)(c->program_index * sizeof(GLTANG_JitProgram) + offsetof(GLTANG_JitProgram, fns)), 64, false));
  B(grjit_builder_load(c->b, c->callee_slot, c->callee_tbl,
      (int32_t)(k * sizeof(GLTANG_JitFn) + offsetof(GLTANG_JitFn, slot)), 64, false));
  B(grjit_builder_call_slot_operand_results_env(c->b, &T[at], 1,
      V(c->callee_slot), GLTANG_FN_WORD(c->program_index, k), V(c->ep), args, (size_t)n + 1u,
      identity_at(c, i), c->state2, c->slots, identity_at(c, i), c->state, c->slots));
}

/** Records where the instruction at `i` is, as the interpreter's SYNC does, for the errors the call makes and for a native's poll. */
static void emit_site(Ctx * c, uint32_t i) {
  B(grjit_builder_store(c->b, c->ep, (int32_t)offsetof(GLTANG_Execution, current_function), 32, I((int64_t)c->function_index)));
  B(grjit_builder_store(c->b, c->ep, (int32_t)offsetof(GLTANG_Execution, current_offset), 32, I((int64_t)i)));
}

/**
 * One call to a native of the table, at `i` with `d` operands before it: the
 * guest frame while it runs and for an exit before it is the one at `i`; the
 * frame after it has the result in the operand slot `result_at` and `after_depth`
 * operands. Everything pending is charged first, since the native may observe it.
 * The position is stored first (`site`) unless the native records it itself, where
 * the interpreter's does only sometimes (a string constant, made on first use).
 */
static void emit_native_at(Ctx * c, uint32_t i, uint32_t d, uint32_t id, const GRJIT_Operand * args, size_t argc, GRJIT_VReg dst, uint32_t after_depth, bool site) {
  if (site) {
    emit_site(c, i);
  }
  flush_cost(c);
  fill_state(c, c->state, i, d);
  fill_state(c, c->state2, i + 1u, after_depth);
  GRJIT_FrameState before = {identity_at(c, i), c->slots, c->state};
  GRJIT_FrameState after = {identity_at(c, i), c->slots, c->state2};
  B(grjit_builder_call_native(c->b, dst, id, args, argc, &before, &after));
  c->native_sites[i >> 3] |= (uint8_t)(1u << (i & 7u));
}

static void emit_native(Ctx * c, uint32_t i, uint32_t d, uint32_t id, const GRJIT_Operand * args, size_t argc, GRJIT_VReg dst, uint32_t after_depth) {
  emit_native_at(c, i, d, id, args, argc, dst, after_depth, true);
}

/**
 * A `CALL n` of the library native `member`: the callee value is guarded to be a
 * native of that member (a mismatch is a guard exit, counted against this
 * function), and the call is `thunk_n(callee, arguments)` through the shared
 * wrapper. A refused call (the native stack would run out) is an exit before it,
 * with nothing charged; a native that reports unwinding, or asks to leave, exits
 * after it with the result in place.
 */
static void emit_native_call(Ctx * c, uint32_t i, uint32_t d, uint32_t n, const GLTANG_LibraryMember * member) {
  GRJIT_VReg * T = c->stack;
  GRJIT_VReg * t = c->tmp;
  const uint32_t at = d - 1u - n;
  bitcast(c, t[TMP_A], T[at]);
  GRJIT_Operand guard_args[2] = {V(t[TMP_A]), I((int64_t)(intptr_t)member)};
  B(grjit_builder_call(c->b, t[TMP_C], (uint64_t)(uintptr_t)gltang_jit_callee_is_native, GRJIT_CALL_NO_GC, GRCORE_SITE_GC_POINT_CALL, guard_args, 2,
      (GRCORE_PollIdentity){0, 0, 0}, NULL, 0));
  GRJIT_BlockId good, bad;
  new_block(c, &good);
  new_block(c, &bad);
  flush_cost(c);
  B(grjit_builder_br_if(c->b, V(t[TMP_C]), good, bad));
  c->open = false;
  set_block(c, bad);
  call_exec_helper(c, (uint64_t)(uintptr_t)gltang_jit_note_native_guard);
  emit_exit(c, i, d);
  set_block(c, good);
  GRJIT_Operand args[GLTANG_JIT_MAX_CALL_ARGS + 1u];
  for (uint32_t j = 0; j <= n; ++j) {
    args[j] = V(T[at + j]);
  }
  emit_native(c, i, d, GLTANG_JIT_NATIVE_CALL0 + n, args, (size_t)n + 1u, T[at], at + 1u);
}

/** `USE k`: the interpreter's resolve, called from compiled code; the value is the operand slot `d`. */
static void emit_use(Ctx * c, uint32_t i, uint32_t d, uint32_t a) {
  set_stack_kind(c, d, KIND_REF);  // the value lands in position d, whatever was left there
  GRJIT_Operand args[1] = {I((int64_t)(intptr_t)c->program->constants[a].block)};
  emit_native(c, i, d, GLTANG_JIT_NATIVE_USE, args, 1, c->stack[d], d + 1u);
}

/** `ATTR name`: the interpreter's attribute rule on the operand in slot `d - 1`, which the value replaces. */
static void emit_attr(Ctx * c, uint32_t i, uint32_t d, uint32_t a) {
  GRJIT_Operand args[2] = {V(c->stack[d - 1u]), I((int64_t)a)};
  emit_native(c, i, d, GLTANG_JIT_NATIVE_ATTR, args, 2, c->stack[d - 1u], d);
}

/*
 * The fast paths (spec-template-compiled story 4d). For a text, a print and a map's attribute,
 * compiled code first makes a plain call (no native-call bracket, no frame state, no GC point) of
 * the vm's second implementation of the operation's common case (helpers.c, over text.c and ops.c),
 * which does the operation only where nothing but its result can follow from it (no poll, no
 * allocation, no delivery to a sink, no failure) and otherwise does nothing; only then does compiled
 * code make the native-table call it made before, the slow path, which is the interpreter's own
 * function. The position is stored before the plain call, as the interpreter's SYNC stores it at the
 * three operations whichever way they go, and the slow call does not store it again. The fuel
 * pending in compiled code is added before the call, and the operation's own cost is charged by
 * whichever path does it.
 */

/** The position, the pending fuel and the plain call `helper(exec, args...)`, its answer in the temporary `TMP_R`. */
static void emit_fast_call(Ctx * c, uint32_t i, uint64_t helper, const GRJIT_Operand * args, size_t argc) {
  GRJIT_Operand all[5];
  assert(argc + 1u <= sizeof(all) / sizeof(all[0]) && "a fast path's helper takes the execution and at most four more");
  all[0] = I((int64_t)(intptr_t)c->exec);
  for (size_t k = 0; k < argc; ++k) {
    all[1u + k] = args[k];
  }
  emit_site(c, i);
  flush_cost(c);
  B(grjit_builder_call(c->b, c->tmp[TMP_R], helper, GRJIT_CALL_NO_GC, GRCORE_SITE_GC_POINT_CALL, all, argc + 1u,
      (GRCORE_PollIdentity){0, 0, 0}, NULL, 0));
}

/** Branches on `taken` to `fast` and otherwise to `slow`, both new, and leaves `slow` current. */
static void branch_fast_or_slow(Ctx * c, GRJIT_Operand taken, GRJIT_BlockId * fast, GRJIT_BlockId * slow) {
  new_block(c, fast);
  new_block(c, slow);
  B(grjit_builder_br_if(c->b, taken, *fast, *slow));
  c->open = false;
  set_block(c, *slow);
}

/**
 * `PRINT`: the interpreter's print of the operand in slot `d - 1`, which its value (null,
 * or the error a failed print makes) replaces. The operand is a value word: a raw float
 * is the designed exit before this (`reads_raw`). A short string whose print cannot poll,
 * allocate or deliver is the fast path, and its value is null as the interpreter's is.
 */
static void emit_print(Ctx * c, uint32_t i, uint32_t d) {
  GRJIT_Operand args[1] = {V(c->stack[d - 1u])};
  GRJIT_BlockId fast, slow, join;
  emit_fast_call(c, i, (uint64_t)(uintptr_t)gltang_jit_print_fast, args, 1);
  branch_fast_or_slow(c, V(c->tmp[TMP_R]), &fast, &slow);
  new_block(c, &join);
  emit_native_at(c, i, d, GLTANG_JIT_NATIVE_PRINT, args, 1, c->stack[d - 1u], d, false);
  branch_to(c, join);
  set_block(c, fast);
  B(grjit_builder_const(c->b, c->stack[d - 1u], (int64_t)GLTANG_V_NULL));
  branch_to(c, join);
  set_block(c, join);
}

/**
 * Whether a text can ever be the fast path: one segment of fewer bytes than a print paces by
 * (`sink_pace`). The rest of the test is made when it runs (gltang_vm_print_constant_fast, which
 * makes this one again), so a text that fails this one is only the call, as before.
 */
static bool text_may_be_fast(const GLTANG_StringBlock * s) {
  return s->segment_count == 1u && s->byte_length < GLTANG_POLL_BYTES;
}

/** `PRINT_CONST k`: the interpreter's print of the constant's text; it pushes nothing, and its value is discarded as the interpreter discards it. */
static void emit_print_const(Ctx * c, uint32_t i, uint32_t d, uint32_t a) {
  // The block is the program's (gcu_calloc'd by compile.c, freed with the program, never in the heap), and the execution holds the program while its code runs.
  const GLTANG_StringBlock * block = c->program->constants[a].block;
  GRJIT_Operand args[1] = {I((int64_t)(intptr_t)block)};
  if (!text_may_be_fast(block)) {
    emit_native(c, i, d, GLTANG_JIT_NATIVE_PRINT_CONST, args, 1, GRJIT_NO_VREG, d);
    return;
  }
  GRJIT_BlockId fast, slow;
  emit_fast_call(c, i, (uint64_t)(uintptr_t)gltang_jit_print_const_fast, args, 1);
  branch_fast_or_slow(c, V(c->tmp[TMP_R]), &fast, &slow);
  emit_native_at(c, i, d, GLTANG_JIT_NATIVE_PRINT_CONST, args, 1, GRJIT_NO_VREG, d, false);
  branch_to(c, fast);  // the fast path does nothing more: both go on from it
  set_block(c, fast);
}

// ---------------------------------------------------------------------------
// The value operations (spec-template-compiled story 3): calls of the interpreter's own
// functions through the native table (natives.c), each a GC point with the frame before the
// operation as its state, behind a test that no operand is a host-defined value.
// ---------------------------------------------------------------------------

/**
 * Leaves compiled code, before the operation at `i` and with nothing of it done, when the
 * operand at stack position `pos` is a host-defined value. The vm functions the helpers call
 * would hand it to its type themselves; the reason is what that is: the type's operation is a
 * host callback (an activation record, a handle scope, `in_host`, and a callback that may later
 * re-enter guest code), which spec-template-compiled does not run from inside a compiled
 * helper. Compiling that dispatch is spec-tang-host-extensibility story 10's
 * (planning/specs/spec-tang-host-extensibility/stories.yaml, id "10"). The word must
 * be a non-null pointer (tag 0) before its kind is read; any other word goes on. The exit is
 * a real one and counts toward the discard limit (the deopt hook reads the refusal).
 */
static void guard_not_custom(Ctx * c, uint32_t i, uint32_t d, uint32_t pos) {
  GRJIT_VReg * t = c->tmp;
  GRJIT_BlockId object, custom, go_on;
  new_block(c, &object);
  new_block(c, &custom);
  new_block(c, &go_on);
  bitcast(c, t[TMP_A], c->stack[pos]);
  tag_is(c, t[TMP_C], t[TMP_A], 0);
  cmp(c, GRJIT_CMP_NE, t[TMP_D], V(t[TMP_A]), I(0));
  bin(c, GRJIT_OP_AND, t[TMP_C], V(t[TMP_C]), V(t[TMP_D]));
  flush_cost(c);
  B(grjit_builder_br_if(c->b, V(t[TMP_C]), object, go_on));
  c->open = false;
  set_block(c, object);
  // Every heap object begins with its 32-bit kind (vm_internal.h, gltang_object_kind).
  B(grjit_builder_load(c->b, t[TMP_X], c->stack[pos], 0, 32, false));
  cmp(c, GRJIT_CMP_EQ, t[TMP_C], V(t[TMP_X]), I((int64_t)GLTANG_OBJ_CUSTOM));
  B(grjit_builder_br_if(c->b, V(t[TMP_C]), custom, go_on));
  c->open = false;
  set_block(c, custom);
  call_exec_helper(c, (uint64_t)(uintptr_t)gltang_jit_note_operand_guard);
  emit_exit(c, i, d);
  set_block(c, go_on);
}

/**
 * The same test of both operands of `+`'s slow path (`d - 2` and `d - 1`), as one call of a
 * no-GC leaf (gltang_jit_operands_custom, which notes the refusal) and one exit. The slow path
 * already calls the interpreter's `+`, so the leaf costs little there, and it keeps every
 * function that adds out of the four blocks and two exits (each with its frame state) the two
 * inline tests made, which every compile of such a function paid for, used or not.
 */
static void guard_neither_custom(Ctx * c, uint32_t i, uint32_t d) {
  GRJIT_BlockId custom, go_on;
  GRJIT_Operand args[3] = {I((int64_t)(intptr_t)c->exec), V(c->stack[d - 2u]), V(c->stack[d - 1u])};
  flush_cost(c);
  B(grjit_builder_call(c->b, c->tmp[TMP_R], (uint64_t)(uintptr_t)gltang_jit_operands_custom, GRJIT_CALL_NO_GC,
      GRCORE_SITE_GC_POINT_CALL, args, 3, (GRCORE_PollIdentity){0, 0, 0}, NULL, 0));
  new_block(c, &custom);
  new_block(c, &go_on);
  B(grjit_builder_br_if(c->b, V(c->tmp[TMP_R]), custom, go_on));
  c->open = false;
  set_block(c, custom);
  emit_exit(c, i, d);
  set_block(c, go_on);
}

/** A string `CONST k`: the execution's cached string, made on first use (the call records the position only then, as the interpreter's SYNC is only then). */
static void emit_string_const(Ctx * c, uint32_t i, uint32_t d, uint32_t a) {
  set_stack_kind(c, d, KIND_REF);  // the value lands in position d, whatever was left there
  GRJIT_Operand args[2] = {I((int64_t)a), I((int64_t)(((uint64_t)c->function_index << 32) | i))};
  emit_native_at(c, i, d, GLTANG_JIT_NATIVE_CONST, args, 2, c->stack[d], d + 1u, false);
}

/** `INDEX`: the interpreter's index of the container in slot `d - 2` by the value in `d - 1`; the element replaces both. */
static void emit_index(Ctx * c, uint32_t i, uint32_t d) {
  guard_not_custom(c, i, d, d - 2u);
  GRJIT_Operand args[2] = {V(c->stack[d - 2u]), V(c->stack[d - 1u])};
  emit_native(c, i, d, GLTANG_JIT_NATIVE_INDEX, args, 2, c->stack[d - 2u], d - 1u);
}

/** `SLICE flags`: the interpreter's slice of the container below its parts (one per flag bit), which the slice replaces. */
static void emit_slice(Ctx * c, uint32_t i, uint32_t d, uint32_t a) {
  const uint32_t parts = (a & 1u) + ((a >> 1) & 1u) + ((a >> 2) & 1u);
  const uint32_t at = d - 1u - parts;
  guard_not_custom(c, i, d, at);
  GRJIT_Operand args[5] = {I((int64_t)(a & 7u)), V(c->stack[at]), I(0), I(0), I(0)};
  for (uint32_t p = 0; p < parts; ++p) {
    args[2u + p] = V(c->stack[at + 1u + p]);
  }
  emit_native(c, i, d, GLTANG_JIT_NATIVE_SLICE, args, 5, c->stack[at], at + 1u);
}

/**
 * `ATTR name` of a value that is not known to be a library: the interpreter's attribute rule; the
 * value replaces the operand. A map, once the name has been made, is the fast path
 * (gltang_vm_map_attr_fast: the key's value, or null), with the name's hash computed here; anything
 * else is the test for a host-defined operand and the call, as before.
 */
static void emit_value_attr(Ctx * c, uint32_t i, uint32_t d, uint32_t a) {
  GRJIT_Operand args[2] = {V(c->stack[d - 1u]), I((int64_t)a)};
  if (!string_constant(c, a)) {
    guard_not_custom(c, i, d, d - 1u);
    emit_native(c, i, d, GLTANG_JIT_NATIVE_VALUE_ATTR, args, 2, c->stack[d - 1u], d);
    return;
  }
  const GLTANG_StringBlock * name = c->program->constants[a].block;
  const uint64_t hash = gltang_vm_string_hash(gltang_string_bytes(name), (size_t)name->byte_length);
  GRJIT_Operand fast_args[4] = {V(c->stack[d - 1u]), I((int64_t)a), I((int64_t)(intptr_t)name), I((int64_t)hash)};
  GRJIT_BlockId fast, slow, join;
  emit_fast_call(c, i, (uint64_t)(uintptr_t)gltang_jit_map_attr_fast, fast_args, 4);
  cmp(c, GRJIT_CMP_NE, c->tmp[TMP_C], V(c->tmp[TMP_R]), I((int64_t)GLTANG_V_UNWIND));
  branch_fast_or_slow(c, V(c->tmp[TMP_C]), &fast, &slow);
  new_block(c, &join);
  guard_not_custom(c, i, d, d - 1u);
  emit_native_at(c, i, d, GLTANG_JIT_NATIVE_VALUE_ATTR, args, 2, c->stack[d - 1u], d, false);
  branch_to(c, join);
  set_block(c, fast);
  bitcast(c, c->stack[d - 1u], c->tmp[TMP_R]);
  branch_to(c, join);
  set_block(c, join);
}

/**
 * The slow path of `+` on two value words, the interpreter's `gltang_vm_op_binary`: where an
 * operand is not a small integer, or the sum leaves the small-integer range, the small-integer
 * path's tests go to `slow` instead of exiting (`guard_to`). There a host-defined operand
 * exits, and anything else (a concatenation of strings or of arrays, a boxed integer, a float
 * object, an error) is the call; both ways join after.
 */
static void begin_add_slow_path(Ctx * c, GRJIT_BlockId * slow) {
  new_block(c, slow);
  c->guard_to = *slow;
  c->guard_to_on = true;
}

static void end_add_slow_path(Ctx * c, uint32_t i, uint32_t d, GRJIT_BlockId slow) {
  GRJIT_BlockId join;
  c->guard_to_on = false;
  new_block(c, &join);
  branch_to(c, join);  // the small-integer path, with its fuel
  set_block(c, slow);
  guard_neither_custom(c, i, d);
  GRJIT_Operand args[2] = {V(c->stack[d - 2u]), V(c->stack[d - 1u])};
  emit_native(c, i, d, GLTANG_JIT_NATIVE_ADD, args, 2, c->stack[d - 2u], d - 1u);
  branch_to(c, join);
  set_block(c, join);
}

// ---------------------------------------------------------------------------
// The value-building operations (spec-template-compiled story 4)
// ---------------------------------------------------------------------------

/** A `CAST` the inline path does not do: the interpreter's cast of the operand in slot `d - 1`, which the result replaces. */
static void emit_cast_call(Ctx * c, uint32_t i, uint32_t d, uint32_t a) {
  GRJIT_Operand args[2] = {V(c->stack[d - 1u]), I((int64_t)a)};
  emit_native(c, i, d, GLTANG_JIT_NATIVE_CAST, args, 2, c->stack[d - 1u], d);
}

/**
 * `ADOPT`: of a container, the interpreter's copy (a call, which allocates and paces); of any other
 * value nothing, inline, as the interpreter does nothing (a host-defined value is stored as a
 * reference). The test is the interpreter's `gltang_vm_is_container`: a non-null pointer whose
 * object's kind word is an array's or a map's.
 */
static void emit_adopt(Ctx * c, uint32_t i, uint32_t d) {
  GRJIT_VReg * t = c->tmp;
  GRJIT_BlockId object, copy, plain, join;
  new_block(c, &object);
  new_block(c, &copy);
  new_block(c, &plain);
  new_block(c, &join);
  bitcast(c, t[TMP_A], c->stack[d - 1u]);
  tag_is(c, t[TMP_C], t[TMP_A], 0);
  cmp(c, GRJIT_CMP_NE, t[TMP_D], V(t[TMP_A]), I(0));
  bin(c, GRJIT_OP_AND, t[TMP_C], V(t[TMP_C]), V(t[TMP_D]));
  flush_cost(c);
  B(grjit_builder_br_if(c->b, V(t[TMP_C]), object, plain));
  c->open = false;
  set_block(c, object);
  // Every heap object begins with its 32-bit kind (vm_internal.h, gltang_object_kind).
  B(grjit_builder_load(c->b, t[TMP_X], c->stack[d - 1u], 0, 32, false));
  cmp(c, GRJIT_CMP_EQ, t[TMP_C], V(t[TMP_X]), I((int64_t)GLTANG_OBJ_ARRAY));
  cmp(c, GRJIT_CMP_EQ, t[TMP_D], V(t[TMP_X]), I((int64_t)GLTANG_OBJ_MAP));
  bin(c, GRJIT_OP_OR, t[TMP_C], V(t[TMP_C]), V(t[TMP_D]));
  B(grjit_builder_br_if(c->b, V(t[TMP_C]), copy, plain));
  c->open = false;
  set_block(c, plain);
  add_fuel(c, gltang_opcode_cost_table[GLTANG_OP_ADOPT]);
  branch_to(c, join);
  set_block(c, copy);
  GRJIT_Operand args[1] = {V(c->stack[d - 1u])};
  emit_native(c, i, d, GLTANG_JIT_NATIVE_ADOPT, args, 1, c->stack[d - 1u], d);  // the thunk charges the fuel
  branch_to(c, join);
  set_block(c, join);
}

/**
 * `ARRAY n` or `MAP n` at `i`, its `count` operands in slots `d - count` to `d - 1`: the native
 * makes the empty container into `built` (a GC point; its frame state before is the interpreter's
 * frame at the literal, with every element on the operand stack), the elements stay in their slot
 * registers, which the stack map names across the call, and the no-GC fill helpers store them, in
 * order, in chunks that fit the helper-call limit; then the container takes the first slot.
 *
 * The frame state after the call names `built` in that slot. The only status the native can report
 * is an unwind (an allocation the memory budget refused with no reserve left), which pops the frame
 * without reading it, so no frame with a container still unfilled is ever rebuilt.
 */
static void emit_literal(Ctx * c, uint32_t i, uint32_t d, uint32_t a, bool map) {
  const uint32_t count = map ? 2u * a : a;
  const uint32_t at = d - count;
  GRJIT_Operand args[6];
  args[0] = I((int64_t)a);
  emit_site(c, i);
  flush_cost(c);
  fill_state(c, c->state, i, d);
  fill_state(c, c->state2, i + 1u, at + 1u);
  c->state2[H + c->lc + at] = grjit_frame_slot_vreg(c->built);
  GRJIT_FrameState before = {identity_at(c, i), c->slots, c->state};
  GRJIT_FrameState after = {identity_at(c, i), c->slots, c->state2};
  B(grjit_builder_call_native(c->b, c->built, map ? GLTANG_JIT_NATIVE_MAP : GLTANG_JIT_NATIVE_ARRAY, args, 1, &before, &after));
  c->native_sites[i >> 3] |= (uint8_t)(1u << (i & 7u));
  const uint32_t chunk = map ? 2u * GLTANG_JIT_FILL_PAIRS : GLTANG_JIT_FILL_ELEMENTS;
  for (uint32_t done = 0; done < count; done += chunk) {
    const uint32_t n = count - done < chunk ? count - done : chunk;
    // The array's count, or the map's absent second key: a 0 (no string is 0).
    size_t argc = 0;
    args[argc++] = I((int64_t)(intptr_t)c->exec);
    args[argc++] = V(c->built);
    if (!map) {
      args[argc++] = I((int64_t)n);
    }
    for (uint32_t k = 0; k < chunk; ++k) {
      args[argc++] = k < n ? V(c->stack[at + done + k]) : I(0);
    }
    // A map's fill returns the literal's value: the map, or the out-of-memory root where a key is
    // that error (gltang_vm_map_fill, shared with the interpreter's MAP). The root is a root, so the
    // word needs no GC point to be held.
    B(grjit_builder_call(c->b, map ? c->built : GRJIT_NO_VREG, map ? (uint64_t)(uintptr_t)gltang_jit_map_fill : (uint64_t)(uintptr_t)gltang_jit_array_fill,
        GRJIT_CALL_NO_GC, GRCORE_SITE_GC_POINT_CALL, args, argc, (GRCORE_PollIdentity){0, 0, 0}, NULL, 0));
  }
  B(grjit_builder_move(c->b, c->stack[at], V(c->built)));
}

static bool emit_op_kinds(Ctx * c, uint32_t i, uint32_t d);

/**
 * One instruction. The kinds at its start are in `lk` and `sk` (loaded from the
 * merged state at a jump target, carried by this function from the instruction
 * before it otherwise); the frame states it builds describe them, and the effect of
 * the instruction on them is applied after.
 */
static void emit_op(Ctx * c, uint32_t i, uint32_t d) {
  // A jump target has the merged kinds the analysis recorded (`kind_at`, made by `reach`
  // for exactly the reachable targets and kept until `free_prod` at the end of the
  // build, which is after every emission); any other instruction carries on from the
  // one before it in code order, which is the only way into it.
  if (c->target[i]) {
    if (!c->kind_at[i]) {
      c->err = GRJIT_ERR_INTERNAL;  // a reachable target the analysis recorded nothing for
      return;
    }
    memcpy(c->lk, c->kind_at[i], c->lc);
    if (d > 0) {
      memcpy(c->sk, c->kind_at[i] + c->lc, d);
    }
  }
  // Only an instruction that flows on changes the kinds; an exit leaves nothing after it.
  if (emit_op_kinds(c, i, d) && !apply_kinds(c, GLTANG_INSTRUCTION_OP(c->fn->code[i]), GLTANG_INSTRUCTION_A(c->fn->code[i]), (int32_t)d)) {
    c->err = GRJIT_ERR_INTERNAL;
  }
}

/** Emits one instruction; true if it flows on, false if it is an exit. */
static bool emit_op_kinds(Ctx * c, uint32_t i, uint32_t d) {
  const uint32_t word = c->fn->code[i];
  const GLTANG_Opcode op = GLTANG_INSTRUCTION_OP(word);
  const uint32_t a = GLTANG_INSTRUCTION_A(word);
  const uint32_t cost = gltang_opcode_cost_table[op];
  GRJIT_VReg * T = c->stack;
  GRJIT_VReg * t = c->tmp;
  int64_t k = 0;

  // What the analysis decided for this instruction and what the emission now sees must be the
  // same: it ends at the designed exit exactly when it reads a raw operand.
  if (reads_raw(c, i, op, a, (int32_t)d) != (c->end_kind[i] == END_EXPECTED)) {
    c->err = GRJIT_ERR_INTERNAL;
    return false;
  }
  if (reads_raw(c, i, op, a, (int32_t)d)) {
    // The raw operand is in the frame state and the interpreter re-executes the instruction. This
    // exit is the design (the float is boxed for its consumer, once, by the rebuild), not a failure
    // of compiled code: the site is flagged so that it does not count toward the discard limit.
    c->expected_sites[i >> 3] |= (uint8_t)(1u << (i & 7u));
    emit_exit(c, i, d);
    return false;
  }
  if (op == GLTANG_OP_CALL) {
    if (c->call_k[i] >= 0) {
      emit_call(c, i, d, a, (uint32_t)c->call_k[i]);
      c->call_sites[i >> 3] |= (uint8_t)(1u << (i & 7u));
      return true;
    }
    if (c->call_member[i]) {
      emit_native_call(c, i, d, a, c->call_member[i]);
      return true;
    }
    emit_exit(c, i, d);
    return false;
  }
  if (op == GLTANG_OP_ATTR) {
    if (c->load_ok[i]) {
      emit_attr(c, i, d, a);
      return true;
    }
    if (c->natives_on) {
      emit_value_attr(c, i, d, a);
      return true;
    }
    emit_exit(c, i, d);
    return false;
  }
  if (op == GLTANG_OP_CAST && a == GLTANG_CAST_TYPE_INTEGER && constant_cast(c, i, &k) == CAST_FOLD_OUT) {
    // A float constant outside the small-integer range (or a NaN): the interpreter makes the boxed
    // integer or the error from the constant, raw in the frame state and boxed by the rebuild. That
    // is a box exit by design, as `reads_raw`'s are: it does not count toward the discard limit, so a
    // function that holds `9223372036854775807.0 as int` is not let go after eight calls. (Its walk
    // ends as END_REAL all the same, so the static decline does not take a function whose one path
    // is this cast: the corpus scripts of these constants stay compiled.)
    c->expected_sites[i >> 3] |= (uint8_t)(1u << (i & 7u));
    emit_exit(c, i, d);
    return false;
  }
  if (!inline_op(c, op, a)) {
    emit_exit(c, i, d);
    return false;
  }
  switch (op) {
    case GLTANG_OP_POLL:
      add_fuel(c, cost);
      emit_poll(c, i, d);
      break;

    case GLTANG_OP_LINE: {
      // A statement boundary: free, and a poll only when the execution asked for
      // statement polls, which is read here at every execution and not when the
      // code was compiled (so a debugger can attach mid-run). The engine never
      // asks whether a debugger is attached.
      GRJIT_BlockId polls, after;
      new_block(c, &polls);
      new_block(c, &after);
      flush_cost(c);
      B(grjit_builder_load(c->b, t[TMP_A], c->ep, (int32_t)offsetof(GLTANG_Execution, statement_polls), 8, false));
      B(grjit_builder_br_if(c->b, V(t[TMP_A]), polls, after));
      c->open = false;
      set_block(c, polls);
      emit_poll(c, i, d);
      branch_to(c, after);
      set_block(c, after);
      break;
    }

    case GLTANG_OP_POP:
      add_fuel(c, cost);
      break;
    case GLTANG_OP_DUP:
      if (c->sk[d - 1u] == KIND_F64) {
        B(grjit_builder_move(c->b, c->stack_f[d], V(c->stack_f[d - 1u])));
      }
      else {
        B(grjit_builder_move(c->b, T[d], V(T[d - 1u])));
      }
      add_fuel(c, cost);
      break;
    case GLTANG_OP_NULL:
      B(grjit_builder_const(c->b, T[d], (int64_t)GLTANG_V_NULL));
      add_fuel(c, cost);
      break;
    case GLTANG_OP_TRUE:
      B(grjit_builder_const(c->b, T[d], (int64_t)GLTANG_V_TRUE));
      add_fuel(c, cost);
      break;
    case GLTANG_OP_FALSE:
      B(grjit_builder_const(c->b, T[d], (int64_t)GLTANG_V_FALSE));
      add_fuel(c, cost);
      break;
    case GLTANG_OP_CONST:
      if (!c->raw_off && float_constant(c, a, &k)) {
        // The interpreter's double, bit for bit, as the register's immediate.
        B(grjit_builder_const(c->b, c->stack_f[d], k));
      }
      else if (!small_int_constant(c, a, &k)) {
        // A string (inline_op takes no other): the call charges its fuel.
        emit_string_const(c, i, d, a);
        break;
      }
      else {
        (void)small_int_constant(c, a, &k);
        B(grjit_builder_const(c->b, T[d], (int64_t)gltang_v_from_small_int(k)));
      }
      add_fuel(c, cost);
      break;
    case GLTANG_OP_FUNC:
      B(grjit_builder_const(c->b, T[d], (int64_t)gltang_v_from_function(a)));
      add_fuel(c, cost);
      break;
    case GLTANG_OP_LOAD_LOCAL:
      if (c->lk[a] == KIND_F64) {
        B(grjit_builder_move(c->b, c->stack_f[d], V(c->local_f[a])));
      }
      else {
        B(grjit_builder_move(c->b, T[d], V(c->local[a])));
      }
      add_fuel(c, cost);
      break;
    case GLTANG_OP_STORE_LOCAL:
      if (c->sk[d - 1u] == KIND_F64) {
        B(grjit_builder_move(c->b, c->local_f[a], V(c->stack_f[d - 1u])));
      }
      else {
        B(grjit_builder_move(c->b, c->local[a], V(T[d - 1u])));
      }
      add_fuel(c, cost);
      break;
    case GLTANG_OP_USE:
      emit_use(c, i, d, a);
      break;
    case GLTANG_OP_INDEX:
      emit_index(c, i, d);
      break;
    case GLTANG_OP_SLICE:
      emit_slice(c, i, d, a);
      break;
    case GLTANG_OP_ADOPT:
      emit_adopt(c, i, d);
      break;
    case GLTANG_OP_ARRAY:
      emit_literal(c, i, d, a, false);
      break;
    case GLTANG_OP_MAP:
      emit_literal(c, i, d, a, true);
      break;

    // The output operations: the thunks charge the two prints' fuel, as the interpreter
    // charges an operation it runs; the clear is charged here, since it cannot leave.
    case GLTANG_OP_PRINT:
      emit_print(c, i, d);
      break;
    case GLTANG_OP_PRINT_CONST:
      emit_print_const(c, i, d, a);
      break;
    case GLTANG_OP_CLEAR_RESULT:
      add_fuel(c, cost);
      call_exec_helper(c, (uint64_t)(uintptr_t)gltang_jit_clear_result);
      break;

    // The statement ends. Neither call can leave or observe the fuel, so each is charged
    // here, as CLEAR_RESULT is; the operand is a value word (a raw float was the box exit).
    case GLTANG_OP_SET_RESULT: {
      bitcast(c, t[TMP_A], T[d - 1u]);
      GRJIT_Operand args[3] = {I((int64_t)(intptr_t)c->exec), V(t[TMP_A]), I((int64_t)(a & 1u))};
      add_fuel(c, cost);
      B(grjit_builder_call(c->b, GRJIT_NO_VREG, (uint64_t)(uintptr_t)gltang_jit_set_result, GRJIT_CALL_NO_GC,
          GRCORE_SITE_GC_POINT_CALL, args, 3, (GRCORE_PollIdentity){0, 0, 0}, NULL, 0));
      break;
    }
    case GLTANG_OP_DISCARD: {
      bitcast(c, t[TMP_A], T[d - 1u]);
      GRJIT_Operand args[4] = {I((int64_t)(intptr_t)c->exec), V(t[TMP_A]), I((int64_t)c->function_index), I((int64_t)i)};
      add_fuel(c, cost);
      B(grjit_builder_call(c->b, GRJIT_NO_VREG, (uint64_t)(uintptr_t)gltang_jit_discard, GRJIT_CALL_NO_GC,
          GRCORE_SITE_GC_POINT_CALL, args, 4, (GRCORE_PollIdentity){0, 0, 0}, NULL, 0));
      break;
    }
    case GLTANG_OP_STORE_GLOBAL:
      // The interpreter's `exec->globals[a] = value`, the running program's variables
      // read at each store as at each load; the value stays on the stack. The array is
      // not a heap object (the execution's roots, or a template activation's calloc'd
      // block, enumerated as roots), so the store is the plain word the interpreter
      // writes, with no barrier: the value is stored as its bits.
      bitcast(c, t[TMP_A], T[d - 1u]);
      B(grjit_builder_load(c->b, c->gp, c->ep, (int32_t)offsetof(GLTANG_Execution, globals), 64, false));
      B(grjit_builder_store(c->b, c->gp, (int32_t)(a * sizeof(GLTANG_Value)), 64, V(t[TMP_A])));
      add_fuel(c, cost);
      break;

    case GLTANG_OP_LOAD_GLOBAL:
      // `exec->globals` is the running program's variables, read at each load as
      // the interpreter reads it.
      B(grjit_builder_load(c->b, c->gp, c->ep, (int32_t)offsetof(GLTANG_Execution, globals), 64, false));
      B(grjit_builder_load(c->b, T[d], c->gp, (int32_t)(a * sizeof(GLTANG_Value)), 64, false));
      add_fuel(c, cost);
      break;

    case GLTANG_OP_NEG:
      if (c->sk[d - 1u] == KIND_F64) {
        // The sign bit alone: -0.0 for 0.0, a NaN keeps its payload.
        B(grjit_builder_unary(c->b, GRJIT_OP_FNEG, c->stack_f[d - 1u], V(c->stack_f[d - 1u])));
        add_fuel(c, cost);
        break;
      }
      bitcast(c, t[TMP_A], T[d - 1u]);
      tag_is(c, t[TMP_C], t[TMP_A], GLTANG_TAG_INTEGER);
      guard(c, V(t[TMP_C]), i, d);
      bin(c, GRJIT_OP_SAR, t[TMP_X], V(t[TMP_A]), I(4));
      B(grjit_builder_unary(c->b, GRJIT_OP_NEG, t[TMP_R], V(t[TMP_X])));
      guard_in_range(c, t[TMP_R], i, d);
      box(c, T[d - 1u], t[TMP_R], GLTANG_TAG_INTEGER);
      add_fuel(c, cost);
      break;

    case GLTANG_OP_NOT:
      bitcast(c, t[TMP_A], T[d - 1u]);
      tag_is(c, t[TMP_C], t[TMP_A], GLTANG_TAG_BOOL);
      guard(c, V(t[TMP_C]), i, d);
      // false 0x02 and true 0x12 differ in one bit: flipping it is `!`.
      bin(c, GRJIT_OP_XOR, t[TMP_R], V(t[TMP_A]), I(0x10));
      bitcast(c, T[d - 1u], t[TMP_R]);
      add_fuel(c, cost);
      break;

    case GLTANG_OP_ADD:
    case GLTANG_OP_SUB:
    case GLTANG_OP_MUL:
    case GLTANG_OP_DIV:
      if (c->sk[d - 2u] == KIND_F64 || c->sk[d - 1u] == KIND_F64) {
        // A float operation: both operands become doubles (an integer by `(double)`, in the
        // order they stand), every guard runs, and only then is the result written to its slot.
        GRJIT_VReg fa = float_operand(c, i, d, d - 2u, c->tmp_f[0]);
        GRJIT_VReg fb = float_operand(c, i, d, d - 1u, c->tmp_f[1]);
        if (op == GLTANG_OP_DIV) {
          // The interpreter's error before it divides: either zero, but not a NaN (`NE` is true for one).
          cmp(c, GRJIT_CMP_NE, t[TMP_C], V(fb), FI(0.0));
          guard(c, V(t[TMP_C]), i, d);
        }
        GRJIT_OpKind fk = op == GLTANG_OP_ADD ? GRJIT_OP_FADD : op == GLTANG_OP_SUB ? GRJIT_OP_FSUB : op == GLTANG_OP_MUL ? GRJIT_OP_FMUL : GRJIT_OP_FDIV;
        B(grjit_builder_binary(c->b, fk, c->stack_f[d - 2u], V(fa), V(fb)));
        add_fuel(c, cost);
        break;
      }
      if (op == GLTANG_OP_MUL) {
        goto integer_mul;
      }

      if (op == GLTANG_OP_DIV) {
        guard_two_ints(c, i, d);
        bin(c, GRJIT_OP_SAR, t[TMP_X], V(t[TMP_A]), I(4));
        bin(c, GRJIT_OP_SAR, t[TMP_Y], V(t[TMP_B]), I(4));
        guard_nonzero(c, t[TMP_Y], i, d);
        // The verifier's second guard: the dividend is not the minimum or the divisor is not -1.
        // It cannot fire for small integers (INT64_MIN is not one), and the quotient's range
        // guard below is what catches SMALL_MIN / -1.
        cmp(c, GRJIT_CMP_NE, t[TMP_C], V(t[TMP_X]), I(INT64_MIN));
        cmp(c, GRJIT_CMP_NE, t[TMP_D], V(t[TMP_Y]), I(-1));
        bin(c, GRJIT_OP_OR, t[TMP_U], V(t[TMP_C]), V(t[TMP_D]));
        guard(c, V(t[TMP_U]), i, d);
        bin(c, GRJIT_OP_DIV_S, t[TMP_R], V(t[TMP_X]), V(t[TMP_Y]));
        guard_in_range(c, t[TMP_R], i, d);
        box(c, T[d - 2u], t[TMP_R], GLTANG_TAG_INTEGER);
        add_fuel(c, cost);
        break;
      }
      {
        // Story 3: for `+`, an operand that is not a small integer, or a sum outside the range,
        // is the interpreter's `+` called from compiled code (a concatenation most often), not
        // an exit; `-` keeps its guards.
        const bool slow_path = op == GLTANG_OP_ADD && c->natives_on;
        GRJIT_BlockId slow = 0;
        if (slow_path) {
          begin_add_slow_path(c, &slow);
        }
        guard_two_ints(c, i, d);
        bin(c, GRJIT_OP_SAR, t[TMP_X], V(t[TMP_A]), I(4));
        bin(c, GRJIT_OP_SAR, t[TMP_Y], V(t[TMP_B]), I(4));
        bin(c, op == GLTANG_OP_ADD ? GRJIT_OP_ADD : GRJIT_OP_SUB, t[TMP_R], V(t[TMP_X]), V(t[TMP_Y]));
        guard_in_range(c, t[TMP_R], i, d);
        box(c, T[d - 2u], t[TMP_R], GLTANG_TAG_INTEGER);
        add_fuel(c, cost);
        if (slow_path) {
          end_add_slow_path(c, i, d, slow);
        }
      }
      break;

    integer_mul:
      guard_two_ints(c, i, d);
      bin(c, GRJIT_OP_SAR, t[TMP_X], V(t[TMP_A]), I(4));
      bin(c, GRJIT_OP_SAR, t[TMP_Y], V(t[TMP_B]), I(4));
      // Both operands in -2^29..2^29-1, so the product is inside 2^58 and
      // cannot leave the small-integer range, and no overflow check follows.
      bin(c, GRJIT_OP_ADD, t[TMP_U], V(t[TMP_X]), I((int64_t)1 << 29));
      cmp(c, GRJIT_CMP_ULT, t[TMP_C], V(t[TMP_U]), I((int64_t)1 << 30));
      bin(c, GRJIT_OP_ADD, t[TMP_U], V(t[TMP_Y]), I((int64_t)1 << 29));
      cmp(c, GRJIT_CMP_ULT, t[TMP_D], V(t[TMP_U]), I((int64_t)1 << 30));
      bin(c, GRJIT_OP_AND, t[TMP_C], V(t[TMP_C]), V(t[TMP_D]));
      guard(c, V(t[TMP_C]), i, d);
      bin(c, GRJIT_OP_MUL, t[TMP_R], V(t[TMP_X]), V(t[TMP_Y]));
      box(c, T[d - 2u], t[TMP_R], GLTANG_TAG_INTEGER);
      add_fuel(c, cost);
      break;

    case GLTANG_OP_LT:
    case GLTANG_OP_LE:
    case GLTANG_OP_GT:
    case GLTANG_OP_GE:
    case GLTANG_OP_EQ:
    case GLTANG_OP_NE: {
      GRJIT_Cmp cond = op == GLTANG_OP_LT ? GRJIT_CMP_LT : op == GLTANG_OP_LE ? GRJIT_CMP_LE
        : op == GLTANG_OP_GT ? GRJIT_CMP_GT : op == GLTANG_OP_GE ? GRJIT_CMP_GE
        : op == GLTANG_OP_EQ ? GRJIT_CMP_EQ : GRJIT_CMP_NE;
      if (c->sk[d - 2u] == KIND_F64 || c->sk[d - 1u] == KIND_F64) {
        // Doubles, both converted: ordered, so a NaN on either side is false for all but `!=`.
        GRJIT_VReg fa = float_operand(c, i, d, d - 2u, c->tmp_f[0]);
        GRJIT_VReg fb = float_operand(c, i, d, d - 1u, c->tmp_f[1]);
        cmp(c, cond, t[TMP_R], V(fa), V(fb));
        box(c, T[d - 2u], t[TMP_R], GLTANG_TAG_BOOL);
        add_fuel(c, cost);
        break;
      }
      guard_two_ints(c, i, d);
      // A tagged integer is `n << 4 | 1`, which keeps the order and the
      // equality of n, so the words compare as the integers do.
      cmp(c, cond, t[TMP_R], V(t[TMP_A]), V(t[TMP_B]));
      box(c, T[d - 2u], t[TMP_R], GLTANG_TAG_BOOL);
      add_fuel(c, cost);
      break;
    }

    case GLTANG_OP_MOD:
      // Integers only (a raw operand was an exit before this): the zero guard, then the
      // remainder, which has the dividend's sign and cannot leave the small-integer range.
      guard_two_ints(c, i, d);
      bin(c, GRJIT_OP_SAR, t[TMP_X], V(t[TMP_A]), I(4));
      bin(c, GRJIT_OP_SAR, t[TMP_Y], V(t[TMP_B]), I(4));
      guard_nonzero(c, t[TMP_Y], i, d);
      bin(c, GRJIT_OP_REM_S, t[TMP_R], V(t[TMP_X]), V(t[TMP_Y]));
      box(c, T[d - 2u], t[TMP_R], GLTANG_TAG_INTEGER);
      add_fuel(c, cost);
      break;

    case GLTANG_OP_CAST:
      if (a == GLTANG_CAST_TYPE_BOOLEAN || a == GLTANG_CAST_TYPE_STRING) {
        emit_cast_call(c, i, d, a);  // the thunk charges the fuel
        break;
      }
      if (constant_cast(c, i, &k) == CAST_FOLD_IN) {
        // A float constant inside the range: the integer is known now (the CONST's raw register is dead).
        B(grjit_builder_const(c->b, T[d - 1u], (int64_t)gltang_v_from_small_int(k)));
        add_fuel(c, cost);
        break;
      }
      if (a == GLTANG_CAST_TYPE_FLOAT) {
        if (c->sk[d - 1u] != KIND_F64) {
          // A small integer is converted and a float object read; the result is the raw double.
          unbox(c, i, d, d - 1u, c->stack_f[d - 1u]);
        }
        add_fuel(c, cost);
        break;
      }
      if (c->sk[d - 1u] == KIND_F64) {
        trunc_small(c, i, d, c->stack_f[d - 1u], t[TMP_R]);
        box(c, T[d - 1u], t[TMP_R], GLTANG_TAG_INTEGER);
        add_fuel(c, cost);
        break;
      }
      else {
        // A small integer is itself; a float object is truncated (inside the range) and tagged.
        // Anything else (a string, null, a boolean, an error, a float outside the range) is the
        // interpreter's cast called from compiled code (story 4), where natives can be called: the
        // tests go to that slow path instead of exiting (`guard_to`), as `+`'s do.
        const bool slow_path = c->natives_on;
        GRJIT_BlockId whole, other, slow = 0;
        new_block(c, &whole);
        new_block(c, &other);
        bitcast(c, t[TMP_A], T[d - 1u]);
        tag_is(c, t[TMP_C], t[TMP_A], GLTANG_TAG_INTEGER);
        flush_cost(c);
        B(grjit_builder_br_if(c->b, V(t[TMP_C]), whole, other));
        c->open = false;
        set_block(c, other);
        if (slow_path) {
          new_block(c, &slow);
          c->guard_to = slow;
          c->guard_to_on = true;
        }
        unbox_float_object(c, i, d, d - 1u, c->tmp_f[0]);
        if (slow_path) {
          // The range test, as a branch to the helper: trunc_small's own guards, which the verifier
          // requires beside the truncation in its block, then always hold.
          cmp(c, GRJIT_CMP_GE, t[TMP_C], V(c->tmp_f[0]), FI(-0x1p59));
          cmp(c, GRJIT_CMP_LT, t[TMP_D], V(c->tmp_f[0]), FI(0x1p59));
          bin(c, GRJIT_OP_AND, t[TMP_C], V(t[TMP_C]), V(t[TMP_D]));
          guard(c, V(t[TMP_C]), i, d);
          c->guard_to_on = false;
        }
        trunc_small(c, i, d, c->tmp_f[0], t[TMP_R]);
        box(c, T[d - 1u], t[TMP_R], GLTANG_TAG_INTEGER);
        branch_to(c, whole);
        set_block(c, whole);
        add_fuel(c, cost);
        if (slow_path) {
          GRJIT_BlockId join;
          new_block(c, &join);
          branch_to(c, join);
          set_block(c, slow);
          emit_cast_call(c, i, d, a);  // the thunk charges the fuel
          branch_to(c, join);
          set_block(c, join);
        }
      }
      break;

    case GLTANG_OP_JMP:
      add_fuel(c, cost);
      branch_to(c, c->block[a]);
      break;

    case GLTANG_OP_JMP_FALSE:
    case GLTANG_OP_JMP_TRUE:
    case GLTANG_OP_AND:
    case GLTANG_OP_OR: {
      bitcast(c, t[TMP_A], T[d - 1u]);
      tag_is(c, t[TMP_C], t[TMP_A], GLTANG_TAG_BOOL);
      guard(c, V(t[TMP_C]), i, d);
      add_fuel(c, cost);
      flush_cost(c);
      bool jump_if_true = op == GLTANG_OP_JMP_TRUE || op == GLTANG_OP_OR;
      cmp(c, GRJIT_CMP_EQ, t[TMP_D], V(t[TMP_A]), I((int64_t)(jump_if_true ? GLTANG_V_TRUE : GLTANG_V_FALSE)));
      B(grjit_builder_br_if(c->b, V(t[TMP_D]), c->block[a], c->block[i + 1u]));
      c->open = false;
      break;
    }

    case GLTANG_OP_RET:
      add_fuel(c, cost);
      flush_cost(c);
      B(grjit_builder_ret(c->b, V(T[d - 1u])));
      c->open = false;
      break;

    case GLTANG_OP_HALT:
      // A return, not an exit: `HALT`'s fuel is charged here, and the halted word names
      // the instruction. The interpreter runs `HALT`'s body once compiled code has
      // returned and its JIT record is left (interp.c, `halt_frame`): the budget scope of a
      // template call cannot close while they are open, and the output string it makes
      // allocates, so compiled code gains no GC point and no unwind here. The register is
      // a reference only so that every return of the function has one type; nothing can
      // collect between the constant and the return.
      // A non-pointer word there is safe for the collector: runtime-jit puts a register in a
      // stack map only if it is live across a site, an operation with a frame state
      // (src/backend/liveness.c), and none lies between this constant and the return; the
      // returned word goes to the entry's result buffer (`grjit_code_call`'s `out`), which is
      // no root, and never into a compiled caller's register, since function 0 is never a
      // compiled callee (no `FUNC 0` names it).
      add_fuel(c, cost);
      flush_cost(c);
      B(grjit_builder_const(c->b, c->halted, (int64_t)gltang_jit_halted_word(i)));
      B(grjit_builder_ret(c->b, V(c->halted)));
      c->open = false;
      break;

    default:
      c->err = GRJIT_ERR_INTERNAL;
      break;
  }
  return c->err == GRJIT_OK;
}

static GLTANG_Result map_result(GRJIT_Result r) {
  switch (r) {
    case GRJIT_OK: return GLTANG_OK;
    case GRJIT_ERR_OOM: return GLTANG_ERR_OOM;
    case GRJIT_ERR_LIMIT: return GLTANG_ERR_LIMIT;
    default: return GLTANG_ERR_INTERNAL;
  }
}

static void release_code(void * payload) {
  GLTANG_JitCode * jc = payload;
  const GRCORE_Allocator * allocator = jc->allocator;
  grjit_code_destroy(jc->code);
  gcu_allocator_free(allocator, jc->call_sites);
  gcu_allocator_free(allocator, jc->native_sites);
  gcu_allocator_free(allocator, jc->expected_sites);
  gcu_allocator_free(allocator, jc);
}

/**
 * Lang-tang's frames hold plain words, values and raw floats: no derived pointer,
 * and the only converting location is a raw `F64` (a double the engine boxes when
 * the frame is rebuilt), on a raw slot, never in a stack map. A site that says
 * otherwise is a defect of this compiler. The most converting locations at any one
 * site is what a frame of this function adds to the reservation.
 */
static bool metadata_is_plain(const GRJIT_Code * code, uint32_t * max_converting) {
  const GRCORE_CodeMeta * meta = grjit_code_meta(code);
  *max_converting = 0;
  for (size_t s = 0; s < meta->site_count; ++s) {
    const GRCORE_CodeSite * site = &meta->sites[s];
    if (site->derived_count != 0) {
      return false;
    }
    for (size_t l = 0; l < site->live_count; ++l) {
      if (site->live[l].representation != GRCORE_REPR_BITS) {
        return false;
      }
    }
    size_t converting = 0;
    for (size_t l = 0; l < site->frame_state_count; ++l) {
      const GRCORE_CodeLocation * loc = &site->frame_state[l];
      if (loc->representation == GRCORE_REPR_BITS) {
        continue;
      }
      if (loc->representation != GRCORE_REPR_F64 || loc->slot_kind != GRCORE_SLOT_RAW) {
        return false;
      }
      ++converting;
    }
    if (converting != grcore_deopt_converting_count(site)) {
      return false;
    }
    if (converting > *max_converting) {
      *max_converting = (uint32_t)converting;
    }
  }
  return true;
}

GLTANG_Result gltang_jit_build(GLTANG_Execution * exec, uint32_t program_index, uint32_t function_index, GRCORE_Code ** out) {
  const GLTANG_Program * program = gltang_exec_program(exec, program_index);
  if (!program || function_index >= program->function_count || !grjit_backend_available()) {
    return GLTANG_ERR_UNSUPPORTED;
  }
  // The generated code finds each callee's entry slot through the environment, and
  // so needs every function of the program to have one before any caller is built.
  if (!gltang_jit_prepare_slots(exec, program_index)) {
    return GLTANG_ERR_OOM;
  }
  const GLTANG_Function * fn = &program->functions[function_index];
  // The shapes that are not worth compiling, which are not failures: a function
  // that does not begin with its entry poll, one with nothing after it, one too
  // big for the frame-state arrays, one that takes more arguments than a compiled
  // call carries, and one whose first operation after the entry poll leaves
  // compiled code at once.
  if (fn->code_count < 2u || GLTANG_INSTRUCTION_OP(fn->code[0]) != GLTANG_OP_POLL || fn->frame_slots > GLTANG_JIT_MAX_SLOTS
      || fn->parameter_count > GLTANG_JIT_MAX_CALL_ARGS) {
    *out = NULL;
    return GLTANG_OK;
  }
  Ctx * c = gcu_allocator_calloc(GLTANG_JIT_ALLOCATOR(exec), 1, sizeof(Ctx));
  if (!c) {
    return GLTANG_ERR_OOM;
  }
  GLTANG_Result result = GLTANG_ERR_OOM;
  GRJIT_Function * ir = NULL;
  GRJIT_Code * code = NULL;
  GLTANG_JitCode * payload = NULL;
  uint8_t * call_sites = NULL;
  uint8_t * native_sites = NULL;
  uint8_t * expected_sites = NULL;
  c->exec = exec;
  c->program = program;
  c->fn = fn;
  c->program_index = program_index;
  c->function_index = function_index;
  c->fword = GLTANG_FN_WORD(program_index, function_index);
  c->params = fn->parameter_count;
  c->lc = fn->local_count;
  c->ms = fn->max_stack;
  c->slots = fn->frame_slots;
  c->err = GRJIT_OK;
  // Calls compile only where compiled recursion cannot fault: the native stack is
  // measured in bytes against the context's budget, and with no budget there is
  // nothing to measure against.
  c->calls_on = !(exec->jit && exec->jit->test_calls_off) && grcore_context_native_stack_bytes(exec->context) != GRCORE_UNLIMITED;
  size_t n = fn->code_count;
  c->depth = gcu_allocator_malloc(GLTANG_JIT_ALLOCATOR(exec), n * sizeof(int32_t));
  c->leader = gcu_allocator_calloc(GLTANG_JIT_ALLOCATOR(exec), n, sizeof(bool));
  c->target = gcu_allocator_calloc(GLTANG_JIT_ALLOCATOR(exec), n, sizeof(bool));
  c->queued = gcu_allocator_calloc(GLTANG_JIT_ALLOCATOR(exec), n, sizeof(bool));
  c->flowed = gcu_allocator_calloc(GLTANG_JIT_ALLOCATOR(exec), n, sizeof(bool));
  c->force_exit = gcu_allocator_calloc(GLTANG_JIT_ALLOCATOR(exec), n, sizeof(bool));
  c->block = gcu_allocator_calloc(GLTANG_JIT_ALLOCATOR(exec), n, sizeof(GRJIT_BlockId));
  c->work = gcu_allocator_malloc(GLTANG_JIT_ALLOCATOR(exec), n * sizeof(uint32_t) + sizeof(uint32_t));
  c->prod_at = gcu_allocator_calloc(GLTANG_JIT_ALLOCATOR(exec), n, sizeof(uint32_t *));
  c->kind_at = gcu_allocator_calloc(GLTANG_JIT_ALLOCATOR(exec), n, sizeof(uint8_t *));
  c->cur = gcu_allocator_calloc(GLTANG_JIT_ALLOCATOR(exec), (size_t)c->ms + 2u, sizeof(uint32_t));
  c->call_k = gcu_allocator_malloc(GLTANG_JIT_ALLOCATOR(exec), n * sizeof(int32_t));
  c->global_fn = gcu_allocator_malloc(GLTANG_JIT_ALLOCATOR(exec), ((size_t)program->global_count + 1u) * sizeof(uint32_t));
  c->local_fn = gcu_allocator_malloc(GLTANG_JIT_ALLOCATOR(exec), ((size_t)c->lc + 1u) * sizeof(uint32_t));
  call_sites = gcu_allocator_calloc(GLTANG_JIT_ALLOCATOR(exec), (n >> 3) + 1u, 1);
  c->call_sites = call_sites;
  native_sites = gcu_allocator_calloc(GLTANG_JIT_ALLOCATOR(exec), (n >> 3) + 1u, 1);
  c->native_sites = native_sites;
  expected_sites = gcu_allocator_calloc(GLTANG_JIT_ALLOCATOR(exec), (n >> 3) + 1u, 1);
  c->expected_sites = expected_sites;
  c->end_kind = gcu_allocator_calloc(GLTANG_JIT_ALLOCATOR(exec), n, sizeof(uint8_t));
  c->call_member = gcu_allocator_calloc(GLTANG_JIT_ALLOCATOR(exec), n, sizeof(const GLTANG_LibraryMember *));
  c->attr_src = gcu_allocator_calloc(GLTANG_JIT_ALLOCATOR(exec), n, sizeof(uint32_t));
  c->str_side = gcu_allocator_calloc(GLTANG_JIT_ALLOCATOR(exec), n, sizeof(uint8_t));
  c->cast_src = gcu_allocator_calloc(GLTANG_JIT_ALLOCATOR(exec), n, sizeof(uint32_t));
  c->load_ok = gcu_allocator_calloc(GLTANG_JIT_ALLOCATOR(exec), n, sizeof(bool));
  c->declined = gcu_allocator_calloc(GLTANG_JIT_ALLOCATOR(exec), n, sizeof(bool));
  c->global_use = gcu_allocator_malloc(GLTANG_JIT_ALLOCATOR(exec), ((size_t)program->global_count + 1u) * sizeof(uint32_t));
  c->local_use = gcu_allocator_malloc(GLTANG_JIT_ALLOCATOR(exec), ((size_t)c->lc + 1u) * sizeof(uint32_t));
  if (!c->depth || !c->leader || !c->target || !c->queued || !c->flowed || !c->force_exit || !c->block || !c->work || !c->prod_at || !c->kind_at
      || !c->cur || !c->call_k || !c->global_fn || !c->local_fn || !call_sites || !native_sites || !expected_sites || !c->end_kind || !c->call_member
      || !c->attr_src || !c->str_side || !c->cast_src || !c->load_ok || !c->declined || !c->global_use || !c->local_use) {
    goto done;
  }
  for (size_t i = 0; i < n; ++i) {
    c->depth[i] = -1;
    c->call_k[i] = -1;
  }
  for (uint32_t g = 0; g < program->global_count; ++g) {
    c->global_fn[g] = DECL_NONE;
  }
  for (uint32_t l = 0; l < c->lc; ++l) {
    c->local_fn[l] = DECL_NONE;
  }
  for (uint32_t g = 0; g < program->global_count; ++g) {
    c->global_use[g] = DECL_NONE;
  }
  for (uint32_t l = 0; l < c->lc; ++l) {
    c->local_use[l] = DECL_NONE;
  }
  // Library calls and member loads are compiled when the execution has the table
  // of natives and the backend calls them; otherwise they stay exits, and the
  // statistic counts the sites that would have been compiled.
  c->natives_on = exec->jit && !exec->jit->test_natives_off && grjit_backend_calls_available() && gltang_jit_natives(exec);
  if (!inline_op(c, GLTANG_INSTRUCTION_OP(fn->code[1]), GLTANG_INSTRUCTION_A(fn->code[1]))) {
    *out = NULL;
    result = GLTANG_OK;
    goto done;
  }
  scan_targets(c);
  if (c->calls_on) {
    scan_declarations(c);
  }
  scan_uses(c);
  result = analyse(c);
  if (result != GLTANG_OK) {
    goto done;
  }
  if (c->raw_off && !inline_op(c, GLTANG_INSTRUCTION_OP(fn->code[1]), GLTANG_INSTRUCTION_A(fn->code[1]))) {
    // A merge disagreed on a kind, so a float constant is an exit again, and the
    // function begins with one: it leaves compiled code at once, as before.
    *out = NULL;
    result = GLTANG_OK;
    goto done;
  }
  // The static decline. A function with no backward jump, no normal return and no real exit, whose
  // every path therefore ends at an exit made only to box a raw float for its consumer, cannot
  // repay its compile: each call runs a few instructions in compiled code, then leaves for the
  // interpreter. Only when every path is provably so; any other function is compiled.
  {
    bool expected_end = false, other_end = false;
    for (size_t i = 0; i < n; ++i) {
      if (c->depth[i] < 0) {
        continue;
      }
      expected_end = expected_end || c->end_kind[i] == END_EXPECTED;
      other_end = other_end || c->end_kind[i] == END_REAL || c->end_kind[i] == END_RETURN;
    }
    if (expected_end && !other_end && !c->back_branch) {
      ++exec->jit_stats.functions_declined_expected;
      *out = NULL;
      result = GLTANG_OK;
      goto done;
    }
  }
  // The F64 temporaries are declared only where the function has a float operation (a slot that is
  // ever raw) or a cast to int of a value word; any other function is emitted as it was.
  for (uint32_t k = 0; k < GLTANG_JIT_MAX_SLOTS && !c->need_tmp_f; ++k) {
    c->need_tmp_f = c->ever_local[k] || c->ever_stack[k];
  }
  for (size_t i = 0; i < n && !c->need_tmp_f; ++i) {
    c->need_tmp_f = c->depth[i] >= 0 && GLTANG_INSTRUCTION_OP(fn->code[i]) == GLTANG_OP_CAST && GLTANG_INSTRUCTION_A(fn->code[i]) == GLTANG_CAST_TYPE_INTEGER;
  }
  for (size_t i = 0; i < n && !c->need_built; ++i) {
    const GLTANG_Opcode op = GLTANG_INSTRUCTION_OP(fn->code[i]);
    c->need_built = c->depth[i] >= 0 && (op == GLTANG_OP_ARRAY || op == GLTANG_OP_MAP);
  }
  for (size_t i = 0; i < n; ++i) {
    if (c->depth[i] >= 0 && c->declined[i]) {
      ++exec->jit_stats.native_sites_unsupported;
    }
  }
  // The function: callable, with its parameters as references and the hidden
  // flag after them, one register per other local and per operand-stack
  // position, the zero register, the flags word, the execution's address, and the
  // temporaries.
  {
    char name[64];
    snprintf(name, sizeof(name), "tang_%u_%u", program_index, function_index);
    GRJIT_Result r = grjit_builder_create(name, c->slots, NULL, GLTANG_JIT_ALLOCATOR(exec), &c->b);
    if (r != GRJIT_OK) {
      result = map_result(r);
      goto done;
    }
  }
  B(grjit_builder_set_callable(c->b, &gltang_jit_hooks, 1));
  if (c->natives_on) {
    B(grjit_builder_set_natives(c->b, exec->jit->natives));
  }
  B(grjit_builder_set_token(c->b, c->fword));
  // The call token includes the program index; runtime-core's guest-frame
  // function ID is the function index within that program.
  B(grjit_builder_set_guest_function(c->b, GLTANG_FN_INDEX(c->fword)));
  for (uint32_t k = 0; k < c->params; ++k) {
    B(grjit_builder_param(c->b, GRJIT_TYPE_REF, &c->local[k]));
  }
  B(grjit_builder_param(c->b, GRJIT_TYPE_I64, &c->flag));
  for (uint32_t k = c->params; k < c->lc; ++k) {
    B(grjit_builder_vreg(c->b, GRJIT_TYPE_REF, &c->local[k]));
  }
  for (uint32_t j = 0; j < c->ms; ++j) {
    B(grjit_builder_vreg(c->b, GRJIT_TYPE_REF, &c->stack[j]));
  }
  // The raw register beside each slot that is ever raw; a frame state names it
  // where the slot's kind is `F64`, and the `REF` one where it is not.
  for (uint32_t k = 0; k < c->lc; ++k) {
    if (c->ever_local[k]) {
      B(grjit_builder_vreg(c->b, GRJIT_TYPE_F64, &c->local_f[k]));
    }
  }
  for (uint32_t j = 0; j < c->ms; ++j) {
    if (c->ever_stack[j]) {
      B(grjit_builder_vreg(c->b, GRJIT_TYPE_F64, &c->stack_f[j]));
    }
  }
  if (c->need_tmp_f) {
    for (int j = 0; j < 2; ++j) {
      B(grjit_builder_vreg(c->b, GRJIT_TYPE_F64, &c->tmp_f[j]));
    }
  }
  B(grjit_builder_vreg(c->b, GRJIT_TYPE_REF, &c->zero));
  B(grjit_builder_vreg(c->b, GRJIT_TYPE_I64, &c->flags));
  B(grjit_builder_vreg(c->b, GRJIT_TYPE_PTR, &c->ep));
  B(grjit_builder_vreg(c->b, GRJIT_TYPE_PTR, &c->act));
  B(grjit_builder_vreg(c->b, GRJIT_TYPE_PTR, &c->gp));
  B(grjit_builder_vreg(c->b, GRJIT_TYPE_REF, &c->halted));
  if (c->need_built) {
    B(grjit_builder_vreg(c->b, GRJIT_TYPE_REF, &c->built));
  }
  B(grjit_builder_vreg(c->b, GRJIT_TYPE_PTR, &c->callee_slot));
  B(grjit_builder_vreg(c->b, GRJIT_TYPE_PTR, &c->callee_tbl));
  B(grjit_builder_vreg(c->b, GRJIT_TYPE_PTR, &c->jitp));
  for (int j = 0; j < TMP_COUNT; ++j) {
    B(grjit_builder_vreg(c->b, GRJIT_TYPE_I64, &c->tmp[j]));
  }
  B(grjit_builder_set_poll_helper(c->b, gltang_jit_poll));

  // Block 0 is the prologue, so a loop whose head is the first operation does
  // not run it again. Then the entry poll of a compiled call, then one block per
  // leader, in code order.
  GRJIT_BlockId prologue;
  new_block(c, &prologue);
  new_block(c, &c->entry_poll);
  for (size_t i = 1; i < n; ++i) {
    if (c->depth[i] >= 0 && c->leader[i]) {
      new_block(c, &c->block[i]);
    }
  }
  set_block(c, prologue);
  kinds_all_ref(c);
  B(grjit_builder_const(c->b, c->zero, 0));
  for (uint32_t k = c->params; k < c->lc; ++k) {
    B(grjit_builder_const(c->b, c->local[k], 0));
  }
  B(grjit_builder_instance(c->b, c->ep));
  B(grjit_builder_load(c->b, c->jitp, c->ep, (int32_t)offsetof(GLTANG_Execution, jit), 64, false));
  B(grjit_builder_load(c->b, c->act, c->ep, (int32_t)offsetof(GLTANG_Execution, act), 64, false));
  B(grjit_builder_load(c->b, c->flags, c->act, (int32_t)offsetof(GLTANG_Activation, depth), 64, false));
  // The interpreter made the entry poll (flag 1); a compiled call did not.
  B(grjit_builder_br_if(c->b, V(c->flag), c->block[1], c->entry_poll));
  c->open = false;
  set_block(c, c->entry_poll);
  add_fuel(c, gltang_opcode_cost_table[GLTANG_OP_POLL]);
  emit_poll(c, 0, 0);
  branch_to(c, c->block[1]);

  for (uint32_t i = 1; i < n && c->err == GRJIT_OK; ++i) {
    if (c->depth[i] < 0) {
      continue;
    }
    if (c->leader[i]) {
      if (c->open) {
        branch_to(c, c->block[i]);
      }
      set_block(c, c->block[i]);
    }
    else if (!c->open) {
      c->err = GRJIT_ERR_INTERNAL;
      break;
    }
    emit_op(c, i, (uint32_t)c->depth[i]);
  }
  if (c->err == GRJIT_OK && c->open) {
    c->err = GRJIT_ERR_INTERNAL;
  }
  if (c->err != GRJIT_OK) {
    result = map_result(c->err);
    goto done;
  }
  {
    GRJIT_Result r = grjit_builder_finish(c->b, &ir);
    if (r != GRJIT_OK) {
      result = map_result(r);
      goto done;
    }
    c->b = NULL;
    GRJIT_CompileOptions options = {0};
    // The group's provider, not the context's: the executable pages are not charged
    // to the guest's memory budget (the statistics count them).
    options.pages = grcore_group_page_provider(grcore_context_group(exec->context));
    options.allocator = GLTANG_JIT_ALLOCATOR(exec);
    r = grjit_compile(&options, ir, &code);
    if (r != GRJIT_OK) {
      result = r == GRJIT_ERR_IO || r == GRJIT_ERR_UNSUPPORTED ? GLTANG_ERR_UNSUPPORTED : map_result(r);
      goto done;
    }
  }
  uint32_t max_converting = 0;
  if (!metadata_is_plain(code, &max_converting)) {
    result = GLTANG_ERR_INTERNAL;
    goto done;
  }
  payload = gcu_allocator_malloc(GLTANG_JIT_ALLOCATOR(exec), sizeof(GLTANG_JitCode));
  if (!payload) {
    result = GLTANG_ERR_OOM;
    goto done;
  }
  payload->code = code;
  payload->allocator = GLTANG_JIT_ALLOCATOR(exec);
  payload->frame_slots = fn->frame_slots;
  payload->local_count = fn->local_count;
  payload->parameter_count = fn->parameter_count;
  payload->code_count = fn->code_count;
  payload->call_sites = call_sites;
  payload->native_sites = native_sites;
  payload->expected_sites = expected_sites;
  payload->max_converting = max_converting;
  {
    GRCORE_Code * handle = NULL;
    if (grcore_code_create(GLTANG_JIT_ALLOCATOR(exec), payload, release_code, &handle) != GRCORE_OK) {
      result = GLTANG_ERR_OOM;
      goto done;
    }
    *out = handle;
    code = NULL;
    payload = NULL;
    call_sites = NULL;
    native_sites = NULL;
    expected_sites = NULL;
    result = GLTANG_OK;
  }

done:
  if (c->b) {
    grjit_builder_destroy(c->b);
  }
  grjit_function_destroy(ir);
  if (code) {
    grjit_code_destroy(code);
  }
  gcu_allocator_free(GLTANG_JIT_ALLOCATOR(exec), payload);
  gcu_allocator_free(GLTANG_JIT_ALLOCATOR(exec), call_sites);
  gcu_allocator_free(GLTANG_JIT_ALLOCATOR(exec), native_sites);
  gcu_allocator_free(GLTANG_JIT_ALLOCATOR(exec), expected_sites);
  gcu_allocator_free(GLTANG_JIT_ALLOCATOR(exec), c->end_kind);
  free_prod(c);
  gcu_allocator_free(GLTANG_JIT_ALLOCATOR(exec), c->call_member);
  gcu_allocator_free(GLTANG_JIT_ALLOCATOR(exec), c->attr_src);
  gcu_allocator_free(GLTANG_JIT_ALLOCATOR(exec), c->str_side);
  gcu_allocator_free(GLTANG_JIT_ALLOCATOR(exec), c->cast_src);
  gcu_allocator_free(GLTANG_JIT_ALLOCATOR(exec), c->load_ok);
  gcu_allocator_free(GLTANG_JIT_ALLOCATOR(exec), c->declined);
  gcu_allocator_free(GLTANG_JIT_ALLOCATOR(exec), c->global_use);
  gcu_allocator_free(GLTANG_JIT_ALLOCATOR(exec), c->local_use);
  gcu_allocator_free(GLTANG_JIT_ALLOCATOR(exec), c->prod_at);
  gcu_allocator_free(GLTANG_JIT_ALLOCATOR(exec), c->kind_at);
  gcu_allocator_free(GLTANG_JIT_ALLOCATOR(exec), c->cur);
  gcu_allocator_free(GLTANG_JIT_ALLOCATOR(exec), c->call_k);
  gcu_allocator_free(GLTANG_JIT_ALLOCATOR(exec), c->global_fn);
  gcu_allocator_free(GLTANG_JIT_ALLOCATOR(exec), c->local_fn);
  gcu_allocator_free(GLTANG_JIT_ALLOCATOR(exec), c->target);
  gcu_allocator_free(GLTANG_JIT_ALLOCATOR(exec), c->queued);
  gcu_allocator_free(GLTANG_JIT_ALLOCATOR(exec), c->flowed);
  gcu_allocator_free(GLTANG_JIT_ALLOCATOR(exec), c->force_exit);
  gcu_allocator_free(GLTANG_JIT_ALLOCATOR(exec), c->depth);
  gcu_allocator_free(GLTANG_JIT_ALLOCATOR(exec), c->leader);
  gcu_allocator_free(GLTANG_JIT_ALLOCATOR(exec), c->block);
  gcu_allocator_free(GLTANG_JIT_ALLOCATOR(exec), c->work);
  gcu_allocator_free(GLTANG_JIT_ALLOCATOR(exec), c);
  return result;
}

/*
 * For the tests only: what the compiler's two tables say about an opcode, in a state
 * where every operand position is raw. Bit 0: an instruction at depth 4 (CALL with two
 * arguments, MAP with two pairs) that reads a raw operand is an exit. Bit 1: the instruction has a known
 * effect on the kinds when it flows on. The test walks every opcode, so one added
 * without an entry in either table is seen. Declared in vm/test_hooks.h.
 */
unsigned gltang_vm_jit_test_opcode_policy(unsigned op) {
  if (op >= (unsigned)GLTANG_OP_COUNT) {
    return 0;
  }
  Ctx c;
  memset(&c, 0, sizeof(c));
  memset(c.sk, KIND_F64, sizeof(c.sk));
  unsigned bits = reads_raw(&c, 0, (GLTANG_Opcode)op, 2, 4) ? 1u : 0u;
  Ctx d;
  memset(&d, 0, sizeof(d));
  d.raw_off = true;  // so that CONST does not read a program this state does not have
  bits |= apply_kinds(&d, (GLTANG_Opcode)op, 0, 3) ? 2u : 0u;
  return bits;
}
