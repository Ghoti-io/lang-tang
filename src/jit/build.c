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
 * What it inlines is the cheap and the exact: small-integer arithmetic and
 * comparisons, booleans, locals and globals, function values, jumps, `POLL`,
 * `LINE`, `RET`, `CALL` of a function the declaration scan can name, and the
 * library: the load of a `use`d path or a library's `.name` and the `CALL` of a
 * library native (a host function), each a call to the engine's own operation
 * (natives.c) with no exit. Each
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
  GRJIT_VReg flag;                ///< The hidden parameter: 1 from the interpreter's entry, 0 from a compiled call.
  GRJIT_VReg zero;                ///< 0, typed as a reference: what a dead operand slot reads as.
  GRJIT_VReg flags;               ///< The frame's flags word (the activation's depth), a plain word.
  GRJIT_VReg ep;                  ///< The execution's address, a plain pointer.
  GRJIT_VReg act;                 ///< The execution's activation, for the prologue.
  GRJIT_VReg gp;                  ///< The globals array, for a global load.
  GRJIT_VReg tmp[TMP_COUNT];
  GRJIT_FrameSlot state[GLTANG_JIT_MAX_SLOTS];
  GRJIT_FrameSlot state2[GLTANG_JIT_MAX_SLOTS];
  uint64_t pending_cost;          ///< Fuel of the operations emitted since the last add to memory.
  bool open;                      ///< The current block has no terminator yet.
  bool oom;                       ///< An analysis allocation failed.
  GRJIT_BlockId entry_poll;       ///< The prologue's branch target when the flag is 0.

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
  bool * load_ok;                 ///< Per index of a USE or ATTR: compiled as a member load.
  bool * declined;                ///< Per index: a library call or member load that natives being unavailable left as an exit.
  uint32_t * global_use;          ///< Per global: the path constant its `use` stores, DECL_NONE or DECL_MANY.
  uint32_t * local_use;           ///< The same for this function's locals.
  uint8_t * native_sites;         ///< One bit per index where a native call (a library call or a member load) is emitted.
  bool natives_on;
  bool * flowed;                  ///< Per index of a CALL: the walk went on past it as a call.
  bool * force_exit;              ///< Per index of a CALL: after a merge made its callee unnamable.
  uint64_t * call_entry;          ///< Per index of a CALL: the address of the callee's slot word.
  uint32_t * global_fn;           ///< Per global: the function its declaration stores, DECL_NONE or DECL_MANY.
  uint32_t * local_fn;            ///< The same for this function's locals.
  uint8_t * call_sites;           ///< One bit per index where a compiled call is emitted.
  bool calls_on;
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

static bool use_load_ok(const Ctx * c, uint32_t constant);

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
    case GLTANG_OP_USE:
      return c->natives_on && use_load_ok(c, a);
    case GLTANG_OP_LOAD_GLOBAL:
      return a < c->program->global_count;
    case GLTANG_OP_FUNC:
      return a < c->program->function_count;
    case GLTANG_OP_CONST:
      return small_int_constant(c, a, &unused);
    default:
      return false;
  }
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
    c->depth[target] = d;
    c->queued[target] = true;
    c->work[(*wn)++] = target;
    return true;
  }
  if (c->depth[target] != d) {
    return false;
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
  if (!reach(c, 1, 0, c->cur, &wn)) {
    return c->oom ? GLTANG_ERR_OOM : GLTANG_ERR_INTERNAL;
  }
  while (wn) {
    uint32_t i = c->work[--wn];
    c->queued[i] = false;
    if (c->depth[i] > 0) {
      memcpy(c->cur, c->prod_at[i], (size_t)c->depth[i] * sizeof(uint32_t));
    }
    for (;;) {
      GLTANG_Opcode op = GLTANG_INSTRUCTION_OP(code[i]);
      uint32_t a = GLTANG_INSTRUCTION_A(code[i]);
      int32_t d = c->depth[i];
      int32_t after = d;
      bool falls = true;
      bool branch = false;       // a conditional or unconditional jump to `a`
      int32_t branch_depth = d;
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
        if (!c->load_ok[i]) {
          break;                 // an unconditional exit: nothing flows from it
        }
        c->flowed[i] = true;
        c->cur[d - 1] = i;
      }
      else if (!inline_op(c, op, a)) {
        if (op == GLTANG_OP_USE && !c->natives_on && use_load_ok(c, a)) {
          c->declined[i] = true;
        }
        break;                   // an unconditional exit: nothing flows from it
      }
      else switch (op) {
        case GLTANG_OP_POLL:
        case GLTANG_OP_LINE:
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
        case GLTANG_OP_NEG:
        case GLTANG_OP_NOT:
          if (d < 1) { return GLTANG_ERR_INTERNAL; }
          c->cur[d - 1] = i;
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
    }
    free_prod(c);
    GLTANG_Result r = analyse_once(c);
    if (r != GLTANG_OK) {
      return r;
    }
    bool again = false;
    for (size_t i = 0; i < c->fn->code_count; ++i) {
      if (!c->flowed[i]) {
        continue;
      }
      bool call = GLTANG_INSTRUCTION_OP(c->fn->code[i]) == GLTANG_OP_CALL;
      if (call ? (c->call_k[i] < 0 && !c->call_member[i]) : !c->load_ok[i]) {
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
  return (GRCORE_PollIdentity){c->fword, index};
}

/**
 * The frame state: the interpreter's frame as it is when the instruction at `pc`
 * is about to run (or has just run, for a poll), in the guest frame's own slot
 * order and exactly `frame_slots` long. The three header words are constants and
 * the flags a register. A local is its register. An operand-stack position below
 * the depth is its register and above it is the zero register, which reads as the
 * zero value slot of the interpreter's frame (a `DEAD` location reads as a raw
 * zero, which is not what the frame holds).
 */
static void fill_state(Ctx * c, GRJIT_FrameSlot * s, uint32_t pc, uint32_t depth) {
  s[GLTANG_F_FUNCTION] = grjit_frame_slot_constant((int64_t)c->fword);
  s[GLTANG_F_PC] = grjit_frame_slot_constant((int64_t)pc);
  s[GLTANG_F_SP] = grjit_frame_slot_constant((int64_t)(H + c->lc + depth));
  s[GLTANG_F_FLAGS] = grjit_frame_slot_vreg(c->flags);
  for (uint32_t k = 0; k < c->lc; ++k) {
    s[H + k] = grjit_frame_slot_vreg(c->local[k]);
  }
  for (uint32_t j = 0; j < c->ms; ++j) {
    s[H + c->lc + j] = grjit_frame_slot_vreg(j < depth ? c->stack[j] : c->zero);
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

static void guard(Ctx * c, GRJIT_Operand cond, uint32_t index, uint32_t depth) {
  flush_cost(c);
  fill_state(c, c->state, index, depth);
  B(grjit_builder_guard(c->b, cond, identity_at(c, index), c->state, c->slots));
}

/** A call to a helper that is not a GC point: `helper(exec)`. */
static void call_exec_helper(Ctx * c, uint64_t helper) {
  GRJIT_Operand args[1] = {I((int64_t)(intptr_t)c->exec)};
  B(grjit_builder_call(c->b, GRJIT_NO_VREG, helper, GRJIT_CALL_NO_GC, GRCORE_SITE_GC_POINT_CALL, args, 1, (GRCORE_PollIdentity){0, 0}, NULL, 0));
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
  B(grjit_builder_call_slot(c->b, T[at], c->call_entry[i], GLTANG_FN_WORD(c->program_index, k), args, (size_t)n + 1u,
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
 */
static void emit_native(Ctx * c, uint32_t i, uint32_t d, uint32_t id, const GRJIT_Operand * args, size_t argc, GRJIT_VReg dst, uint32_t after_depth) {
  emit_site(c, i);
  flush_cost(c);
  fill_state(c, c->state, i, d);
  fill_state(c, c->state2, i + 1u, after_depth);
  GRJIT_FrameState before = {identity_at(c, i), c->slots, c->state};
  GRJIT_FrameState after = {identity_at(c, i), c->slots, c->state2};
  B(grjit_builder_call_native(c->b, dst, id, args, argc, &before, &after));
  c->native_sites[i >> 3] |= (uint8_t)(1u << (i & 7u));
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
      (GRCORE_PollIdentity){0, 0}, NULL, 0));
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
  GRJIT_Operand args[1] = {I((int64_t)(intptr_t)c->program->constants[a].block)};
  emit_native(c, i, d, GLTANG_JIT_NATIVE_USE, args, 1, c->stack[d], d + 1u);
}

/** `ATTR name`: the interpreter's attribute rule on the operand in slot `d - 1`, which the value replaces. */
static void emit_attr(Ctx * c, uint32_t i, uint32_t d, uint32_t a) {
  GRJIT_Operand args[2] = {V(c->stack[d - 1u]), I((int64_t)a)};
  emit_native(c, i, d, GLTANG_JIT_NATIVE_ATTR, args, 2, c->stack[d - 1u], d);
}

static void emit_op(Ctx * c, uint32_t i, uint32_t d) {
  const uint32_t word = c->fn->code[i];
  const GLTANG_Opcode op = GLTANG_INSTRUCTION_OP(word);
  const uint32_t a = GLTANG_INSTRUCTION_A(word);
  const uint32_t cost = gltang_opcode_cost_table[op];
  GRJIT_VReg * T = c->stack;
  GRJIT_VReg * t = c->tmp;
  int64_t k = 0;

  if (op == GLTANG_OP_CALL) {
    if (c->call_k[i] >= 0) {
      emit_call(c, i, d, a, (uint32_t)c->call_k[i]);
      c->call_sites[i >> 3] |= (uint8_t)(1u << (i & 7u));
    }
    else if (c->call_member[i]) {
      emit_native_call(c, i, d, a, c->call_member[i]);
    }
    else {
      emit_exit(c, i, d);
    }
    return;
  }
  if (op == GLTANG_OP_ATTR) {
    if (c->load_ok[i]) {
      emit_attr(c, i, d, a);
    }
    else {
      emit_exit(c, i, d);
    }
    return;
  }
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
    case GLTANG_OP_FUNC:
      B(grjit_builder_const(c->b, T[d], (int64_t)gltang_v_from_function(a)));
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
    case GLTANG_OP_USE:
      emit_use(c, i, d, a);
      break;

    case GLTANG_OP_LOAD_GLOBAL:
      // `exec->globals` is the running program's variables, read at each load as
      // the interpreter reads it.
      B(grjit_builder_load(c->b, c->gp, c->ep, (int32_t)offsetof(GLTANG_Execution, globals), 64, false));
      B(grjit_builder_load(c->b, T[d], c->gp, (int32_t)(a * sizeof(GLTANG_Value)), 64, false));
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
  gcu_allocator_free(allocator, jc->call_sites);
  gcu_allocator_free(allocator, jc->native_sites);
  gcu_allocator_free(allocator, jc);
}

/**
 * Lang-tang's frames hold plain words and values: no derived pointer and no
 * converting representation, which is why the reservation each compiled call
 * extends is always zero. A site that says otherwise is a defect of this compiler.
 */
static bool metadata_is_plain(const GRJIT_Code * code) {
  const GRCORE_CodeMeta * meta = grjit_code_meta(code);
  for (size_t s = 0; s < meta->site_count; ++s) {
    if (meta->sites[s].derived_count != 0 || grcore_deopt_converting_count(&meta->sites[s]) != 0) {
      return false;
    }
  }
  return true;
}

GLTANG_Result gltang_jit_build(GLTANG_Execution * exec, uint32_t program_index, uint32_t function_index, GRCORE_Code ** out) {
  const GLTANG_Program * program = gltang_exec_program(exec, program_index);
  if (!program || function_index >= program->function_count || !grjit_backend_available()) {
    return GLTANG_ERR_UNSUPPORTED;
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
  c->cur = gcu_allocator_calloc(GLTANG_JIT_ALLOCATOR(exec), (size_t)c->ms + 2u, sizeof(uint32_t));
  c->call_k = gcu_allocator_malloc(GLTANG_JIT_ALLOCATOR(exec), n * sizeof(int32_t));
  c->call_entry = gcu_allocator_calloc(GLTANG_JIT_ALLOCATOR(exec), n, sizeof(uint64_t));
  c->global_fn = gcu_allocator_malloc(GLTANG_JIT_ALLOCATOR(exec), ((size_t)program->global_count + 1u) * sizeof(uint32_t));
  c->local_fn = gcu_allocator_malloc(GLTANG_JIT_ALLOCATOR(exec), ((size_t)c->lc + 1u) * sizeof(uint32_t));
  call_sites = gcu_allocator_calloc(GLTANG_JIT_ALLOCATOR(exec), (n >> 3) + 1u, 1);
  c->call_sites = call_sites;
  native_sites = gcu_allocator_calloc(GLTANG_JIT_ALLOCATOR(exec), (n >> 3) + 1u, 1);
  c->native_sites = native_sites;
  c->call_member = gcu_allocator_calloc(GLTANG_JIT_ALLOCATOR(exec), n, sizeof(const GLTANG_LibraryMember *));
  c->attr_src = gcu_allocator_calloc(GLTANG_JIT_ALLOCATOR(exec), n, sizeof(uint32_t));
  c->load_ok = gcu_allocator_calloc(GLTANG_JIT_ALLOCATOR(exec), n, sizeof(bool));
  c->declined = gcu_allocator_calloc(GLTANG_JIT_ALLOCATOR(exec), n, sizeof(bool));
  c->global_use = gcu_allocator_malloc(GLTANG_JIT_ALLOCATOR(exec), ((size_t)program->global_count + 1u) * sizeof(uint32_t));
  c->local_use = gcu_allocator_malloc(GLTANG_JIT_ALLOCATOR(exec), ((size_t)c->lc + 1u) * sizeof(uint32_t));
  if (!c->depth || !c->leader || !c->target || !c->queued || !c->flowed || !c->force_exit || !c->block || !c->work || !c->prod_at
      || !c->cur || !c->call_k || !c->call_entry || !c->global_fn || !c->local_fn || !call_sites || !native_sites || !c->call_member
      || !c->attr_src || !c->load_ok || !c->declined || !c->global_use || !c->local_use) {
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
  c->natives_on = exec->jit && exec->jit->natives && !exec->jit->test_natives_off && grjit_backend_calls_available();
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
  for (size_t i = 0; i < n; ++i) {
    if (c->depth[i] >= 0 && c->declined[i]) {
      ++exec->jit_stats.native_sites_unsupported;
    }
  }
  // The entry slot of each callee a call goes through, named (and made) now.
  for (size_t i = 0; i < n; ++i) {
    if (c->depth[i] >= 0 && c->flowed[i] && c->call_k[i] >= 0) {
      GRCORE_EntrySlot * slot = gltang_jit_slot_for(exec, GLTANG_FN_WORD(program_index, (uint32_t)c->call_k[i]));
      if (!slot) {
        result = GLTANG_ERR_OOM;
        goto done;
      }
      c->call_entry[i] = (uint64_t)(uintptr_t)&slot->entry;
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
  B(grjit_builder_set_callable(c->b, &gltang_jit_hooks));
  if (c->natives_on) {
    B(grjit_builder_set_natives(c->b, exec->jit->natives));
  }
  B(grjit_builder_set_token(c->b, c->fword));
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
  B(grjit_builder_vreg(c->b, GRJIT_TYPE_REF, &c->zero));
  B(grjit_builder_vreg(c->b, GRJIT_TYPE_I64, &c->flags));
  B(grjit_builder_vreg(c->b, GRJIT_TYPE_PTR, &c->ep));
  B(grjit_builder_vreg(c->b, GRJIT_TYPE_PTR, &c->act));
  B(grjit_builder_vreg(c->b, GRJIT_TYPE_PTR, &c->gp));
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
  B(grjit_builder_const(c->b, c->zero, 0));
  for (uint32_t k = c->params; k < c->lc; ++k) {
    B(grjit_builder_const(c->b, c->local[k], 0));
  }
  B(grjit_builder_const(c->b, c->ep, (int64_t)(intptr_t)exec));
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
  if (!metadata_is_plain(code)) {
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
  free_prod(c);
  gcu_allocator_free(GLTANG_JIT_ALLOCATOR(exec), c->call_member);
  gcu_allocator_free(GLTANG_JIT_ALLOCATOR(exec), c->attr_src);
  gcu_allocator_free(GLTANG_JIT_ALLOCATOR(exec), c->load_ok);
  gcu_allocator_free(GLTANG_JIT_ALLOCATOR(exec), c->declined);
  gcu_allocator_free(GLTANG_JIT_ALLOCATOR(exec), c->global_use);
  gcu_allocator_free(GLTANG_JIT_ALLOCATOR(exec), c->local_use);
  gcu_allocator_free(GLTANG_JIT_ALLOCATOR(exec), c->prod_at);
  gcu_allocator_free(GLTANG_JIT_ALLOCATOR(exec), c->cur);
  gcu_allocator_free(GLTANG_JIT_ALLOCATOR(exec), c->call_k);
  gcu_allocator_free(GLTANG_JIT_ALLOCATOR(exec), c->call_entry);
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
