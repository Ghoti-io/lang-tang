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
 * Opcode names and fuel costs.
 */

#include <ghoti.io/lang-tang/macros.h>

#include <ghoti.io/lang-tang/bytecode.h>

static const char * const opcode_names[GLTANG_OP_COUNT] = {
  [GLTANG_OP_HALT] = "HALT",
  [GLTANG_OP_POLL] = "POLL",
  [GLTANG_OP_POP] = "POP",
  [GLTANG_OP_DUP] = "DUP",
  [GLTANG_OP_NULL] = "NULL",
  [GLTANG_OP_TRUE] = "TRUE",
  [GLTANG_OP_FALSE] = "FALSE",
  [GLTANG_OP_CONST] = "CONST",
  [GLTANG_OP_LOAD_LOCAL] = "LOAD_LOCAL",
  [GLTANG_OP_STORE_LOCAL] = "STORE_LOCAL",
  [GLTANG_OP_LOAD_GLOBAL] = "LOAD_GLOBAL",
  [GLTANG_OP_STORE_GLOBAL] = "STORE_GLOBAL",
  [GLTANG_OP_FUNC] = "FUNC",
  [GLTANG_OP_SET_RESULT] = "SET_RESULT",
  [GLTANG_OP_CLEAR_RESULT] = "CLEAR_RESULT",
  [GLTANG_OP_USE] = "USE",
  [GLTANG_OP_NEG] = "NEG",
  [GLTANG_OP_NOT] = "NOT",
  [GLTANG_OP_ADD] = "ADD",
  [GLTANG_OP_SUB] = "SUB",
  [GLTANG_OP_MUL] = "MUL",
  [GLTANG_OP_DIV] = "DIV",
  [GLTANG_OP_MOD] = "MOD",
  [GLTANG_OP_LT] = "LT",
  [GLTANG_OP_LE] = "LE",
  [GLTANG_OP_GT] = "GT",
  [GLTANG_OP_GE] = "GE",
  [GLTANG_OP_EQ] = "EQ",
  [GLTANG_OP_NE] = "NE",
  [GLTANG_OP_JMP] = "JMP",
  [GLTANG_OP_JMP_FALSE] = "JMP_FALSE",
  [GLTANG_OP_JMP_TRUE] = "JMP_TRUE",
  [GLTANG_OP_AND] = "AND",
  [GLTANG_OP_OR] = "OR",
  [GLTANG_OP_CAST] = "CAST",
  [GLTANG_OP_INDEX] = "INDEX",
  [GLTANG_OP_ATTR] = "ATTR",
  [GLTANG_OP_SLICE] = "SLICE",
  [GLTANG_OP_SET_INDEX] = "SET_INDEX",
  [GLTANG_OP_SET_ATTR] = "SET_ATTR",
  [GLTANG_OP_ADOPT] = "ADOPT",
  [GLTANG_OP_ARRAY] = "ARRAY",
  [GLTANG_OP_MAP] = "MAP",
  [GLTANG_OP_CALL] = "CALL",
  [GLTANG_OP_RET] = "RET",
  [GLTANG_OP_PRINT] = "PRINT",
  [GLTANG_OP_PRINT_CONST] = "PRINT_CONST",
  [GLTANG_OP_ITER_INIT] = "ITER_INIT",
  [GLTANG_OP_ITER_NEXT] = "ITER_NEXT",
  [GLTANG_OP_DISCARD] = "DISCARD",
  [GLTANG_OP_LINE] = "LINE",
};

const char * gltang_opcode_name(GLTANG_Opcode op) {
  if ((unsigned)op >= (unsigned)GLTANG_OP_COUNT || !opcode_names[op]) {
    return "?";
  }
  return opcode_names[op];
}

/*
 * The fuel cost of each instruction (AD-21). One for every opcode today. The
 * interpreter reads this table directly, so changing a cost here changes it on
 * every execution of every program identically; a bulk operation is not
 * charged here but by the runtime poll, in proportion to the work it does.
 */
const uint32_t gltang_opcode_cost_table[GLTANG_OP_COUNT] = {
  [GLTANG_OP_HALT] = 1,
  [GLTANG_OP_POLL] = 1,
  [GLTANG_OP_POP] = 1,
  [GLTANG_OP_DUP] = 1,
  [GLTANG_OP_NULL] = 1,
  [GLTANG_OP_TRUE] = 1,
  [GLTANG_OP_FALSE] = 1,
  [GLTANG_OP_CONST] = 1,
  [GLTANG_OP_LOAD_LOCAL] = 1,
  [GLTANG_OP_STORE_LOCAL] = 1,
  [GLTANG_OP_LOAD_GLOBAL] = 1,
  [GLTANG_OP_STORE_GLOBAL] = 1,
  [GLTANG_OP_FUNC] = 1,
  [GLTANG_OP_SET_RESULT] = 1,
  [GLTANG_OP_CLEAR_RESULT] = 1,
  [GLTANG_OP_USE] = 1,
  [GLTANG_OP_NEG] = 1,
  [GLTANG_OP_NOT] = 1,
  [GLTANG_OP_ADD] = 1,
  [GLTANG_OP_SUB] = 1,
  [GLTANG_OP_MUL] = 1,
  [GLTANG_OP_DIV] = 1,
  [GLTANG_OP_MOD] = 1,
  [GLTANG_OP_LT] = 1,
  [GLTANG_OP_LE] = 1,
  [GLTANG_OP_GT] = 1,
  [GLTANG_OP_GE] = 1,
  [GLTANG_OP_EQ] = 1,
  [GLTANG_OP_NE] = 1,
  [GLTANG_OP_JMP] = 1,
  [GLTANG_OP_JMP_FALSE] = 1,
  [GLTANG_OP_JMP_TRUE] = 1,
  [GLTANG_OP_AND] = 1,
  [GLTANG_OP_OR] = 1,
  [GLTANG_OP_CAST] = 1,
  [GLTANG_OP_INDEX] = 1,
  [GLTANG_OP_ATTR] = 1,
  [GLTANG_OP_SLICE] = 1,
  [GLTANG_OP_SET_INDEX] = 1,
  [GLTANG_OP_SET_ATTR] = 1,
  [GLTANG_OP_ADOPT] = 1,
  [GLTANG_OP_ARRAY] = 1,
  [GLTANG_OP_MAP] = 1,
  [GLTANG_OP_CALL] = 1,
  [GLTANG_OP_RET] = 1,
  [GLTANG_OP_PRINT] = 1,
  [GLTANG_OP_PRINT_CONST] = 1,
  [GLTANG_OP_ITER_INIT] = 1,
  [GLTANG_OP_ITER_NEXT] = 1,
  [GLTANG_OP_DISCARD] = 1,
  [GLTANG_OP_LINE] = 0, // a statement boundary: free, so fuel does not depend on the option (design.md)
};

uint32_t gltang_opcode_cost(GLTANG_Opcode op) {
  if ((unsigned)op >= (unsigned)GLTANG_OP_COUNT) {
    return 0;
  }
  return gltang_opcode_cost_table[op];
}
