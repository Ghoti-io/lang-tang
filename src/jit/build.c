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
 * The compiled function takes the frame's locals as its parameters (all
 * references), keeps one virtual register per local and one per operand-stack
 * position, and a running fuel count in one more. It starts "after the entry
 * poll": bytecode index 1, an empty operand stack.
 *
 * What it inlines is the cheap and the exact: small-integer arithmetic and
 * comparisons, booleans, locals, jumps, `POLL`, `LINE` and `RET`. Each step
 * mirrors the interpreter's own inline path, so a guard that holds computes the
 * word the interpreter would. For everything else, and for an operand that is
 * not what the inline path takes, the function leaves through a guard (a
 * deoptimization exit) with the interpreter's exact frame as its frame state,
 * and the interpreter re-executes the operation whole.
 *
 * Fuel is the same on every tier: every executed bytecode costs
 * `gltang_opcode_cost_table[op]`. The running count is flushed to the
 * execution by helper calls before every poll and on `RET`, and by the
 * deoptimizer on exit. A guard is tested before its operation's cost is added,
 * so an operation that deoptimizes has not been charged and the interpreter
 * charges it when it runs it.
 */

#include <ghoti.io/lang-tang/macros.h>

#include "jit_internal.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <ghoti.io/cutil/memory.h>

#define H GLTANG_FRAME_HEADER

/** @brief The temporaries: all plain integers, reused by every operation. */
enum { TMP_A, TMP_B, TMP_X, TMP_Y, TMP_R, TMP_C, TMP_D, TMP_U, TMP_COUNT };

typedef struct Ctx {
  GLTANG_Execution * exec;
  const GLTANG_Program * program;
  const GLTANG_Function * fn;
  uint32_t function_index;
  uint64_t fword;
  uint32_t lc;                    ///< local_count
  uint32_t ms;                    ///< max_stack
  uint32_t slots;                 ///< frame_slots + 1: the interpreter's slots and the fuel
  GRJIT_Builder * b;
  GRJIT_Result err;               ///< The first builder error, sticky.
  GRJIT_VReg local[GLTANG_JIT_MAX_SLOTS];
  GRJIT_VReg stack[GLTANG_JIT_MAX_SLOTS];
  GRJIT_VReg fuel;                ///< Fuel counted and not yet charged.
  GRJIT_VReg ep;                  ///< The execution's address, a plain pointer.
  GRJIT_VReg tmp[TMP_COUNT];
  GRJIT_FrameSlot state[GLTANG_JIT_MAX_SLOTS + 1u];
  int32_t * depth;                ///< Per bytecode index: the operand-stack depth there, or -1 if unreachable.
  bool * leader;
  GRJIT_BlockId * block;
  uint32_t * work;
  bool open;                      ///< The current block has no terminator yet.
} Ctx;

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

/** Whether an instruction is compiled inline, or is a deoptimization exit. */
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
    case GLTANG_OP_CONST:
      return small_int_constant(c, a, &unused);
    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// The stack-depth analysis over the code reachable through compiled paths
// ---------------------------------------------------------------------------

static bool reach(Ctx * c, uint32_t target, int32_t d, size_t * wn) {
  if (target >= c->fn->code_count) {
    return false;
  }
  if (c->depth[target] < 0) {
    c->depth[target] = d;
    c->work[(*wn)++] = target;
    return true;
  }
  return c->depth[target] == d;
}

static GLTANG_Result analyse(Ctx * c) {
  const uint32_t * code = c->fn->code;
  size_t wn = 0;
  c->leader[1] = true;
  if (!reach(c, 1, 0, &wn)) {
    return GLTANG_ERR_INTERNAL;
  }
  while (wn) {
    uint32_t i = c->work[--wn];
    for (;;) {
      GLTANG_Opcode op = GLTANG_INSTRUCTION_OP(code[i]);
      uint32_t a = GLTANG_INSTRUCTION_A(code[i]);
      int32_t d = c->depth[i];
      int32_t after = d;
      bool falls = true;
      bool branch = false;       // a conditional or unconditional jump to `a`
      int32_t branch_depth = d;
      if (!inline_op(c, op, a)) {
        break;                   // an unconditional exit: nothing flows from it
      }
      switch (op) {
        case GLTANG_OP_POLL:
        case GLTANG_OP_LINE:
          break;
        case GLTANG_OP_POP:
          if (d < 1) { return GLTANG_ERR_INTERNAL; }
          after = d - 1;
          break;
        case GLTANG_OP_DUP:
          if (d < 1) { return GLTANG_ERR_INTERNAL; }
          after = d + 1;
          break;
        case GLTANG_OP_NULL:
        case GLTANG_OP_TRUE:
        case GLTANG_OP_FALSE:
        case GLTANG_OP_CONST:
          after = d + 1;
          break;
        case GLTANG_OP_LOAD_LOCAL:
          if (a >= c->lc) { return GLTANG_ERR_INTERNAL; }
          after = d + 1;
          break;
        case GLTANG_OP_STORE_LOCAL:
          if (a >= c->lc || d < 1) { return GLTANG_ERR_INTERNAL; }
          break;
        case GLTANG_OP_NEG:
        case GLTANG_OP_NOT:
          if (d < 1) { return GLTANG_ERR_INTERNAL; }
          break;
        case GLTANG_OP_ADD:
        case GLTANG_OP_SUB:
        case GLTANG_OP_MUL:
        case GLTANG_OP_LT:
        case GLTANG_OP_LE:
        case GLTANG_OP_GT:
        case GLTANG_OP_GE:
        case GLTANG_OP_EQ:
        case GLTANG_OP_NE:
          if (d < 2) { return GLTANG_ERR_INTERNAL; }
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
          falls = false;
          break;
        default:
          return GLTANG_ERR_INTERNAL;
      }
      if (after > (int32_t)c->ms) {
        return GLTANG_ERR_INTERNAL;
      }
      if (branch) {
        if (a >= c->fn->code_count) { return GLTANG_ERR_INTERNAL; }
        c->leader[a] = true;
        if (!reach(c, a, branch_depth, &wn)) { return GLTANG_ERR_INTERNAL; }
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
      if (c->depth[next] >= 0) {
        if (c->depth[next] != after) { return GLTANG_ERR_INTERNAL; }
        break;
      }
      c->depth[next] = after;
      i = next;
    }
  }
  return GLTANG_OK;
}

// ---------------------------------------------------------------------------
// Emission
// ---------------------------------------------------------------------------

static GRCORE_PollIdentity identity_at(const Ctx * c, uint32_t index) {
  return (GRCORE_PollIdentity){c->fword, index};
}

/**
 * The frame state: the interpreter's frame as it is when the instruction at
 * `pc` is about to run (or has just run, for a poll), in the guest frame's own
 * slot order. The three header words are constants, the flags are dead, a local
 * is its register, an operand-stack position below the depth is its register
 * and above it is dead, and the last slot is the fuel counted and not charged.
 */
static void fill_state(Ctx * c, uint32_t pc, uint32_t depth, bool fuel_charged) {
  GRJIT_FrameSlot * s = c->state;
  s[GLTANG_F_FUNCTION] = grjit_frame_slot_constant((int64_t)c->fword);
  s[GLTANG_F_PC] = grjit_frame_slot_constant((int64_t)pc);
  s[GLTANG_F_SP] = grjit_frame_slot_constant((int64_t)(H + c->lc + depth));
  s[GLTANG_F_FLAGS] = grjit_frame_slot_dead();
  for (uint32_t k = 0; k < c->lc; ++k) {
    s[H + k] = grjit_frame_slot_vreg(c->local[k]);
  }
  for (uint32_t j = 0; j < c->ms; ++j) {
    s[H + c->lc + j] = j < depth ? grjit_frame_slot_vreg(c->stack[j]) : grjit_frame_slot_dead();
  }
  s[c->slots - 1u] = fuel_charged ? grjit_frame_slot_constant(0) : grjit_frame_slot_vreg(c->fuel);
}

static void guard(Ctx * c, GRJIT_Operand cond, uint32_t index, uint32_t depth) {
  fill_state(c, index, depth, false);
  B(grjit_builder_guard(c->b, cond, identity_at(c, index), c->state, c->slots));
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

static void add_fuel(Ctx * c, uint32_t cost) {
  if (cost) {
    bin(c, GRJIT_OP_ADD, c->fuel, V(c->fuel), I((int64_t)cost));
  }
}

/** A call to a helper that is not a GC point: `helper(exec, fuel)`. */
static void call_fuel_helper(Ctx * c, uint64_t helper) {
  GRJIT_Operand args[2] = {I((int64_t)(intptr_t)c->exec), V(c->fuel)};
  B(grjit_builder_call(c->b, GRJIT_NO_VREG, helper, GRJIT_CALL_NO_GC, GRCORE_SITE_GC_POINT_CALL, args, 2, (GRCORE_PollIdentity){0, 0}, NULL, 0));
}

/** The charge, the flush and the poll of the interpreter's POLL (and LINE). */
static void emit_poll(Ctx * c, uint32_t index, uint32_t depth) {
  call_fuel_helper(c, (uint64_t)(uintptr_t)gltang_jit_flush);
  B(grjit_builder_const(c->b, c->fuel, 0));
  fill_state(c, index + 1u, depth, true);
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
  B(grjit_builder_br(c->b, id));
  c->open = false;
}

/** An unconditional deoptimization exit at `index`: the interpreter runs it. */
static void emit_exit(Ctx * c, uint32_t index, uint32_t depth) {
  guard(c, I(0), index, depth);
  B(grjit_builder_ret(c->b, grjit_operand_none()));
  c->open = false;
}

static void emit_op(Ctx * c, uint32_t i, uint32_t d) {
  const uint32_t word = c->fn->code[i];
  const GLTANG_Opcode op = GLTANG_INSTRUCTION_OP(word);
  const uint32_t a = GLTANG_INSTRUCTION_A(word);
  const uint32_t cost = gltang_opcode_cost_table[op];
  GRJIT_VReg * T = c->stack;
  GRJIT_VReg * t = c->tmp;
  int64_t k = 0;

  if (!inline_op(c, op, a)) {
    emit_exit(c, i, d);
    return;
  }
  switch (op) {
    case GLTANG_OP_POLL:
      add_fuel(c, cost);
      emit_poll(c, i, d);
      break;

    case GLTANG_OP_LINE: {
      // A statement boundary: free, and a poll only when the execution asked
      // for statement polls, which is read here at every execution and not
      // when the code was compiled (so a debugger can attach mid-run). The
      // engine never asks whether a debugger is attached.
      GRJIT_BlockId polls, after;
      new_block(c, &polls);
      new_block(c, &after);
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
      B(grjit_builder_move(c->b, T[d], V(T[d - 1u])));
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
      (void)small_int_constant(c, a, &k);
      B(grjit_builder_const(c->b, T[d], (int64_t)gltang_v_from_small_int(k)));
      add_fuel(c, cost);
      break;
    case GLTANG_OP_LOAD_LOCAL:
      B(grjit_builder_move(c->b, T[d], V(c->local[a])));
      add_fuel(c, cost);
      break;
    case GLTANG_OP_STORE_LOCAL:
      B(grjit_builder_move(c->b, c->local[a], V(T[d - 1u])));
      add_fuel(c, cost);
      break;

    case GLTANG_OP_NEG:
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
      guard_two_ints(c, i, d);
      bin(c, GRJIT_OP_SAR, t[TMP_X], V(t[TMP_A]), I(4));
      bin(c, GRJIT_OP_SAR, t[TMP_Y], V(t[TMP_B]), I(4));
      bin(c, op == GLTANG_OP_ADD ? GRJIT_OP_ADD : GRJIT_OP_SUB, t[TMP_R], V(t[TMP_X]), V(t[TMP_Y]));
      guard_in_range(c, t[TMP_R], i, d);
      box(c, T[d - 2u], t[TMP_R], GLTANG_TAG_INTEGER);
      add_fuel(c, cost);
      break;

    case GLTANG_OP_MUL:
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
      guard_two_ints(c, i, d);
      // A tagged integer is `n << 4 | 1`, which keeps the order and the
      // equality of n, so the words compare as the integers do.
      GRJIT_Cmp cond = op == GLTANG_OP_LT ? GRJIT_CMP_LT : op == GLTANG_OP_LE ? GRJIT_CMP_LE
        : op == GLTANG_OP_GT ? GRJIT_CMP_GT : op == GLTANG_OP_GE ? GRJIT_CMP_GE
        : op == GLTANG_OP_EQ ? GRJIT_CMP_EQ : GRJIT_CMP_NE;
      cmp(c, cond, t[TMP_R], V(t[TMP_A]), V(t[TMP_B]));
      box(c, T[d - 2u], t[TMP_R], GLTANG_TAG_BOOL);
      add_fuel(c, cost);
      break;
    }

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
      bool jump_if_true = op == GLTANG_OP_JMP_TRUE || op == GLTANG_OP_OR;
      cmp(c, GRJIT_CMP_EQ, t[TMP_D], V(t[TMP_A]), I((int64_t)(jump_if_true ? GLTANG_V_TRUE : GLTANG_V_FALSE)));
      B(grjit_builder_br_if(c->b, V(t[TMP_D]), c->block[a], c->block[i + 1u]));
      c->open = false;
      break;
    }

    case GLTANG_OP_RET:
      add_fuel(c, cost);
      call_fuel_helper(c, (uint64_t)(uintptr_t)gltang_jit_charge);
      B(grjit_builder_ret(c->b, V(T[d - 1u])));
      c->open = false;
      break;

    default:
      c->err = GRJIT_ERR_INTERNAL;
      break;
  }
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
  gcu_allocator_free(allocator, jc);
}

GLTANG_Result gltang_jit_build(GLTANG_Execution * exec, uint32_t program_index, uint32_t function_index, GRCORE_Code ** out) {
  const GLTANG_Program * program = gltang_exec_program(exec, program_index);
  if (!program || function_index >= program->function_count || !grjit_backend_available()) {
    return GLTANG_ERR_UNSUPPORTED;
  }
  const GLTANG_Function * fn = &program->functions[function_index];
  // The shapes that are not worth compiling, which are not failures: a function
  // that does not begin with its entry poll, one with nothing after it, one too
  // big for the frame-state arrays, and one whose first operation after the
  // entry poll leaves compiled code at once.
  if (fn->code_count < 2u || GLTANG_INSTRUCTION_OP(fn->code[0]) != GLTANG_OP_POLL || fn->frame_slots + 1u > GLTANG_JIT_MAX_SLOTS) {
    *out = NULL;
    return GLTANG_OK;
  }
  Ctx * c = gcu_allocator_calloc(exec->allocator, 1, sizeof(Ctx));
  if (!c) {
    return GLTANG_ERR_OOM;
  }
  GLTANG_Result result = GLTANG_ERR_OOM;
  GRJIT_Function * ir = NULL;
  GRJIT_Code * code = NULL;
  GLTANG_JitCode * payload = NULL;
  c->exec = exec;
  c->program = program;
  c->fn = fn;
  c->function_index = function_index;
  c->fword = GLTANG_FN_WORD(program_index, function_index);
  c->lc = fn->local_count;
  c->ms = fn->max_stack;
  c->slots = fn->frame_slots + 1u;
  c->err = GRJIT_OK;
  size_t n = fn->code_count;
  c->depth = gcu_allocator_malloc(exec->allocator, n * sizeof(int32_t));
  c->leader = gcu_allocator_calloc(exec->allocator, n, sizeof(bool));
  c->block = gcu_allocator_calloc(exec->allocator, n, sizeof(GRJIT_BlockId));
  c->work = gcu_allocator_malloc(exec->allocator, n * sizeof(uint32_t) + sizeof(uint32_t));
  if (!c->depth || !c->leader || !c->block || !c->work) {
    goto done;
  }
  for (size_t i = 0; i < n; ++i) {
    c->depth[i] = -1;
  }
  if (!inline_op(c, GLTANG_INSTRUCTION_OP(fn->code[1]), GLTANG_INSTRUCTION_A(fn->code[1]))) {
    *out = NULL;
    result = GLTANG_OK;
    goto done;
  }
  result = analyse(c);
  if (result != GLTANG_OK) {
    goto done;
  }

  // The function: the locals arrive as references, one register per local and
  // per operand-stack position, a fuel count, the execution's address, and the
  // temporaries.
  {
    char name[64];
    snprintf(name, sizeof(name), "tang_%u_%u", program_index, function_index);
    GRJIT_Result r = grjit_builder_create(name, c->slots, NULL, exec->allocator, &c->b);
    if (r != GRJIT_OK) {
      result = map_result(r);
      goto done;
    }
  }
  for (uint32_t k = 0; k < c->lc; ++k) {
    B(grjit_builder_param(c->b, GRJIT_TYPE_REF, &c->local[k]));
  }
  for (uint32_t j = 0; j < c->ms; ++j) {
    B(grjit_builder_vreg(c->b, GRJIT_TYPE_REF, &c->stack[j]));
  }
  B(grjit_builder_vreg(c->b, GRJIT_TYPE_I64, &c->fuel));
  B(grjit_builder_vreg(c->b, GRJIT_TYPE_PTR, &c->ep));
  for (int j = 0; j < TMP_COUNT; ++j) {
    B(grjit_builder_vreg(c->b, GRJIT_TYPE_I64, &c->tmp[j]));
  }
  B(grjit_builder_set_poll_helper(c->b, gltang_jit_poll));

  // Block 0 is the prologue, so a loop whose head is the first operation does
  // not run it again. Then one block per leader, in code order.
  GRJIT_BlockId prologue;
  new_block(c, &prologue);
  for (size_t i = 1; i < n; ++i) {
    if (c->depth[i] >= 0 && c->leader[i]) {
      new_block(c, &c->block[i]);
    }
  }
  set_block(c, prologue);
  B(grjit_builder_const(c->b, c->fuel, 0));
  B(grjit_builder_const(c->b, c->ep, (int64_t)(intptr_t)exec));
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
    options.pages = grcore_context_page_provider(exec->context);
    options.allocator = exec->allocator;
    r = grjit_compile(&options, ir, &code);
    if (r != GRJIT_OK) {
      result = r == GRJIT_ERR_IO || r == GRJIT_ERR_UNSUPPORTED ? GLTANG_ERR_UNSUPPORTED : map_result(r);
      goto done;
    }
  }
  payload = gcu_allocator_malloc(exec->allocator, sizeof(GLTANG_JitCode));
  if (!payload) {
    result = GLTANG_ERR_OOM;
    goto done;
  }
  payload->code = code;
  payload->allocator = exec->allocator;
  payload->frame_slots = fn->frame_slots;
  payload->local_count = fn->local_count;
  {
    GRCORE_Code * handle = NULL;
    if (grcore_code_create(exec->allocator, payload, release_code, &handle) != GRCORE_OK) {
      result = GLTANG_ERR_OOM;
      goto done;
    }
    *out = handle;
    code = NULL;
    payload = NULL;
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
  gcu_allocator_free(exec->allocator, payload);
  gcu_allocator_free(exec->allocator, c->depth);
  gcu_allocator_free(exec->allocator, c->leader);
  gcu_allocator_free(exec->allocator, c->block);
  gcu_allocator_free(exec->allocator, c->work);
  gcu_allocator_free(exec->allocator, c);
  return result;
}
