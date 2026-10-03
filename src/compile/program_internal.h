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
 * The inside of a program: what the compiler builds and the interpreter reads.
 *
 * Immutable after compilation (AD-22), so it needs no lock: the reference
 * count is the only field that changes.
 */

#ifndef GHOTI_IO_GLTANG_COMPILE_PROGRAM_INTERNAL_H
#define GHOTI_IO_GLTANG_COMPILE_PROGRAM_INTERNAL_H

#include <ghoti.io/lang-tang/macros.h>

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <ghoti.io/lang-tang/program.h>

/** @brief What a pool constant is. */
typedef enum {
  GLTANG_CONST_INTEGER = 0, ///< `integer`.
  GLTANG_CONST_FLOAT,       ///< `number`.
  GLTANG_CONST_STRING       ///< `block`, a flat string (string_layout.h).
} GLTANG_ConstKind;

/** @brief One constant, as plain C data. */
typedef struct GLTANG_Const {
  GLTANG_ConstKind kind;
  int64_t integer;
  double number;
  void * block;      ///< A flat string; owned.
  size_t block_size; ///< Its size in bytes.
} GLTANG_Const;

/** @brief The first instruction at or after which a source line applies. */
typedef struct GLTANG_LineEntry {
  uint32_t pc;
  uint32_t line;
} GLTANG_LineEntry;

/** @brief Words of header in every frame: function, pc, sp, flags. */
#define GLTANG_FRAME_HEADER 4u

/** @brief One function. */
typedef struct GLTANG_Function {
  char * name;                 ///< Owned.
  uint32_t parameter_count;
  uint32_t local_count;        ///< Parameters first, then locals and hidden slots.
  uint32_t max_stack;
  uint32_t frame_slots;        ///< GLTANG_FRAME_HEADER + locals + max_stack.
  uint32_t * code;             ///< Owned.
  uint32_t code_count;
  GLTANG_LineEntry * lines;    ///< Owned; ascending by pc.
  uint32_t line_count;
  char ** local_names;         ///< Owned; one per local, NULL for a hidden slot.
} GLTANG_Function;

/** @brief The program. */
struct GLTANG_Program {
  atomic_size_t references;
  char * file;                  ///< Owned.
  GLTANG_Function * functions;  ///< Owned; [0] is the top level.
  uint32_t function_count;
  GLTANG_Const * constants;     ///< Owned.
  uint32_t constant_count;
  char ** global_names;         ///< Owned; the program-scope variables.
  uint32_t global_count;
};

/** @brief Frees every part of a program and the program. */
void gltang_program_free(GLTANG_Program * program);

#endif /* GHOTI_IO_GLTANG_COMPILE_PROGRAM_INTERNAL_H */
