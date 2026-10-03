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
 * @file bytecode.h
 * @stability free
 *
 * lang-tang's own bytecode: the opcodes, the instruction encoding and the fuel
 * cost of each instruction (AD-9, AD-21).
 *
 * Nothing here is ctang's. The set was designed for this engine, from the
 * language reference, as a stack machine whose frames live on the context's
 * guest stack. It is `free` until the library's first tag (AD-14), and then
 * opcodes are only ever added: a number is never reused and never changes its
 * meaning. (`SET_RESULT` takes an operand since the host API: 1 marks the
 * statement's value as one that is listed if it is an error that is replaced.) documentation/design.md has the table with each opcode's stack
 * effect.
 *
 * An instruction is one 32-bit word: the opcode in the low 8 bits and an
 * unsigned operand `a` in the high 24. A function's poll identity is the
 * pair (function index, instruction index of the poll).
 */

#ifndef GHOTI_IO_GLTANG_BYTECODE_H
#define GHOTI_IO_GLTANG_BYTECODE_H

#include <ghoti.io/lang-tang/macros.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The opcodes. Stack effects are written `before -- after`. */
typedef enum {
  GLTANG_OP_HALT = 0,   ///< End the program (function 0 only): pops its frame.
  GLTANG_OP_POLL,       ///< Poll: function entry and every loop back-edge.
  GLTANG_OP_POP,        ///< `v --`.
  GLTANG_OP_DUP,        ///< `v -- v v`.
  GLTANG_OP_NULL,       ///< `-- null`.
  GLTANG_OP_TRUE,       ///< `-- true`.
  GLTANG_OP_FALSE,      ///< `-- false`.
  GLTANG_OP_CONST,      ///< `-- k`: constant `a` of the program's pool.
  GLTANG_OP_LOAD_LOCAL, ///< `-- v`: frame local `a`.
  GLTANG_OP_STORE_LOCAL, ///< `v -- v`: frame local `a`.
  GLTANG_OP_LOAD_GLOBAL, ///< `-- v`: program-scope variable `a`.
  GLTANG_OP_STORE_GLOBAL, ///< `v -- v`: program-scope variable `a`.
  GLTANG_OP_FUNC,       ///< `-- f`: the function value of function `a`.
  GLTANG_OP_SET_RESULT, ///< `v --`: the program's result becomes `v`.
  GLTANG_OP_CLEAR_RESULT, ///< The program's result becomes null.
  GLTANG_OP_USE,        ///< `-- v`: resolve the library path of constant `a`.
  GLTANG_OP_NEG,        ///< `v -- -v`.
  GLTANG_OP_NOT,        ///< `v -- !v`.
  GLTANG_OP_ADD,        ///< `a b -- a+b`.
  GLTANG_OP_SUB,        ///< `a b -- a-b`.
  GLTANG_OP_MUL,        ///< `a b -- a*b`.
  GLTANG_OP_DIV,        ///< `a b -- a/b`.
  GLTANG_OP_MOD,        ///< `a b -- a%b`.
  GLTANG_OP_LT,         ///< `a b -- a<b`.
  GLTANG_OP_LE,         ///< `a b -- a<=b`.
  GLTANG_OP_GT,         ///< `a b -- a>b`.
  GLTANG_OP_GE,         ///< `a b -- a>=b`.
  GLTANG_OP_EQ,         ///< `a b -- a==b`.
  GLTANG_OP_NE,         ///< `a b -- a!=b`.
  GLTANG_OP_JMP,        ///< Jump to instruction `a`.
  GLTANG_OP_JMP_FALSE,  ///< `v --`: jump to `a` if `v` is falsy.
  GLTANG_OP_JMP_TRUE,   ///< `v --`: jump to `a` if `v` is truthy.
  GLTANG_OP_AND,        ///< `v -- v` and jump to `a` if falsy; `v --` otherwise.
  GLTANG_OP_OR,         ///< `v -- v` and jump to `a` if truthy; `v --` otherwise.
  GLTANG_OP_CAST,       ///< `v -- c`: to int, float, bool, string (`a` is GLTANG_Cast_Type).
  GLTANG_OP_INDEX,      ///< `c i -- v`.
  GLTANG_OP_ATTR,       ///< `c -- v`: attribute named by constant `a`.
  GLTANG_OP_SLICE,      ///< `c [s] [e] [t] -- v`: `a` bit 0 start, 1 end, 2 step.
  GLTANG_OP_SET_INDEX,  ///< `c i v -- v`; `a` is 1 when `v` is copied first if it is a container.
  GLTANG_OP_SET_ATTR,   ///< `c v -- v`: member named by constant `a >> 1`; bit 0 as for SET_INDEX.
  GLTANG_OP_ADOPT,      ///< `v -- v'`: a deep copy of a container, else `v`.
  GLTANG_OP_ARRAY,      ///< `e1..en -- array`, `a` = n.
  GLTANG_OP_MAP,        ///< `k1 v1..kn vn -- map`, `a` = n.
  GLTANG_OP_CALL,       ///< `f a1..an -- v`, `a` = n.
  GLTANG_OP_RET,        ///< `v --`: return `v` to the caller.
  GLTANG_OP_PRINT,      ///< `v -- null`: append `v` to the output.
  GLTANG_OP_PRINT_CONST, ///< Append string constant `a` to the output.
  GLTANG_OP_ITER_INIT,  ///< `v -- bool`: start iterating array `v` over locals `a`, `a+1`.
  GLTANG_OP_ITER_NEXT,  ///< `-- e` or jump: two words, the second is the exhausted target.
  GLTANG_OP_DISCARD,    ///< `v --`: drop the value of an expression statement; an error that nothing holds is entered in the error list.
  GLTANG_OP_COUNT       ///< Not an opcode: closes the enum.
} GLTANG_Opcode;

/** @brief The largest operand an instruction can carry. */
#define GLTANG_OPERAND_MAX 0xFFFFFFu

/** @brief Builds an instruction from an opcode and an operand. */
#define GLTANG_INSTRUCTION(op, a) ((uint32_t)(op) | ((uint32_t)(a) << 8))
/** @brief The opcode of an instruction. */
#define GLTANG_INSTRUCTION_OP(i) ((GLTANG_Opcode)((i) & 0xFFu))
/** @brief The operand of an instruction. */
#define GLTANG_INSTRUCTION_A(i) ((uint32_t)((i) >> 8))

/**
 * @brief The name of an opcode, for a disassembly.
 *
 * @param op An opcode.
 * @return A static string; "?" for a value outside the enum.
 */
GLTANG_API const char * gltang_opcode_name(GLTANG_Opcode op);

/**
 * @brief The fuel an executed instruction costs (AD-21).
 *
 * The same on every execution of the same program, whatever the tier. One
 * for every opcode today; a bulk operation (string building, array repetition,
 * slicing, printing a large container) charges more through the runtime poll,
 * in proportion to the work it does.
 *
 * @param op An opcode.
 * @return The cost; zero for a value outside the enum.
 */
GLTANG_API uint32_t gltang_opcode_cost(GLTANG_Opcode op);

/** @brief Bytes of string copied per fuel unit by a bulk operation. */
#define GLTANG_WORK_BYTES_PER_FUEL 64u
/** @brief Elements handled per fuel unit by a bulk operation. */
#define GLTANG_WORK_ELEMENTS_PER_FUEL 4u
/** @brief Bytes a native copies between two runtime polls. */
#define GLTANG_POLL_BYTES 4096u
/** @brief Elements a native handles between two runtime polls. */
#define GLTANG_POLL_ELEMENTS 512u

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GLTANG_BYTECODE_H */
