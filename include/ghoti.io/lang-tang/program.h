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
 * @file program.h
 * @stability free
 *
 * A compiled program: immutable, reference-counted and shared (AD-22).
 *
 * A program holds functions (bytecode, a line table that maps each offset to
 * a source line, the names of the locals), a pool of constants as plain C
 * data, and the names of the program-scope variables. It is never written
 * after it is compiled, so any number of contexts, on any number of threads,
 * can run one. The heap values a run needs are created per context.
 *
 * Function 0 is the program's top level. Its locals are not named variables:
 * a top-level variable lives in the program scope.
 */

#ifndef GHOTI_IO_GLTANG_PROGRAM_H
#define GHOTI_IO_GLTANG_PROGRAM_H

#include <ghoti.io/lang-tang/macros.h>

#include <ghoti.io/lang-tang/core.h>

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A compiled program. Opaque. */
typedef struct GLTANG_Program GLTANG_Program;

/**
 * @brief Takes another reference.
 *
 * Safe to call from any thread.
 *
 * @param program The program, or NULL.
 * @return `program`.
 */
GLTANG_API GLTANG_Program * gltang_program_retain(GLTANG_Program * program);

/**
 * @brief Gives a reference back; the last one frees the program.
 *
 * Safe to call from any thread.
 *
 * @param program The program, or NULL (ignored).
 */
GLTANG_API void gltang_program_release(GLTANG_Program * program);

/**
 * @brief The source file name the program was compiled with.
 *
 * @param program The program.
 * @return A string owned by the program; NULL for NULL.
 */
GLTANG_API const char * gltang_program_file(const GLTANG_Program * program);

/**
 * @brief How many functions the program has, the top level included.
 *
 * @param program The program.
 * @return The count; 0 for NULL.
 */
GLTANG_API size_t gltang_program_function_count(const GLTANG_Program * program);

/**
 * @brief The name of a function.
 *
 * @param program The program.
 * @param function A function index.
 * @return The name, owned by the program; "<program>" for the top level; NULL
 *   for an index out of range.
 */
GLTANG_API const char * gltang_program_function_name(
    const GLTANG_Program * program, size_t function);

/**
 * @brief How many instruction words a function has.
 *
 * @param program The program.
 * @param function A function index.
 * @return The count; 0 for an index out of range.
 */
GLTANG_API size_t gltang_program_function_size(
    const GLTANG_Program * program, size_t function);

/**
 * @brief The maximum operand-stack depth of a function, as the compiler worked
 *   it out.
 *
 * @param program The program.
 * @param function A function index.
 * @return The depth; 0 for an index out of range.
 */
GLTANG_API size_t gltang_program_function_max_stack(
    const GLTANG_Program * program, size_t function);

/**
 * @brief Names a poll identity as a source line.
 *
 * @param program The program.
 * @param function The function index of the identity.
 * @param offset The instruction index of the identity.
 * @param out_line Receives the 1-based line. Written only on success.
 * @return ::GLTANG_OK, or ::GLTANG_ERR_INVALID for NULL or a place that is
 *   not in the program.
 */
GLTANG_API GLTANG_Result gltang_program_locate(const GLTANG_Program * program,
    uint64_t function, uint64_t offset, int * out_line);

/**
 * @brief Writes a disassembly of the program.
 *
 * @param program The program.
 * @param out The stream.
 */
GLTANG_API void gltang_program_dump(const GLTANG_Program * program, FILE * out);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GLTANG_PROGRAM_H */
