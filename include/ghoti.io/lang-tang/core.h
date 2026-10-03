/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
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
 * @file core.h
 * @stability stable
 *
 * Result codes and the version this build reports.
 */

#ifndef GHOTI_IO_GLTANG_CORE_H
#define GHOTI_IO_GLTANG_CORE_H

#include <ghoti.io/lang-tang/allocator.h>
#include <ghoti.io/lang-tang/macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Result of an operation.
 *
 * Zero is success.  ::GLTANG_RESULT_COUNT closes the enum so a test can check
 * the string table is complete.
 *
 * This is the suite's fixed vocabulary, numbered as in every other library and
 * as in runtime-heap's ::GRHEAP_Result. It has no ERR_GUEST: that constant is
 * runtime-core's, for a guest program's uncaught error, and arrives here with
 * execution. A syntax error in the source is ::GLTANG_ERR_FORMAT.
 */
typedef enum {
  GLTANG_OK = 0,          ///< The operation succeeded.
  GLTANG_ERR_IO,          ///< A read, write, or seek failed.
  GLTANG_ERR_FORMAT,      ///< Well-formed bytes, but not a format handled here.
  GLTANG_ERR_UNSUPPORTED, ///< The format is known and the feature is not
                          ///< implemented.
  GLTANG_ERR_LIMIT,       ///< A stated cap was exceeded.
  GLTANG_ERR_CORRUPT,     ///< The bytes are not a valid encoding of this
                          ///< format.
  GLTANG_ERR_OOM,         ///< The allocator returned NULL.
  GLTANG_ERR_INVALID,     ///< A caller-supplied argument is wrong.
  GLTANG_ERR_INTERNAL,    ///< The library's own invariant failed.
  GLTANG_RESULT_COUNT
} GLTANG_Result;

/**
 * @brief Static description of a result code.
 *
 * The string is never NULL, never allocated, and never contains caller data.
 *
 * @param result The result code, including values outside the enum.
 * @return A static string.
 */
GLTANG_API const char * gltang_result_string(GLTANG_Result result);

/**
 * @brief This build's version, as the string the Makefile generated.
 *
 * @return A static string, never NULL.  "0.0.0", "0.0.0-dev" when BRANCH
 *   was overridden, and with "-debug" appended for a BUILD=debug build
 *   ("0.0.0-debug", "0.0.0-dev-debug").
 */
GLTANG_API const char * gltang_version_string(void);

/**
 * @brief This build's version, packed as ::GLTANG_MAKE_VERSION packs it.
 *
 * @return `(major << 16) | (minor << 8) | patch`.
 */
GLTANG_API unsigned gltang_version_number(void);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GLTANG_CORE_H */
