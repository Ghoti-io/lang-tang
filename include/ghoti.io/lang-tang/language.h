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
 * @file language.h
 * @stability stable
 *
 * The language layer: libraries every execution in the process can `use`.
 *
 * A `use` looks its first name up in four places, in order: the execution's
 * libraries (::gltang_execution_set_libraries), the program's, **the language
 * layer**, and the built-ins (`math`, `random`). The first that has it wins, so
 * a host adds a library for every program once, and a program or an execution
 * can still shadow it.
 *
 * The layer is one sealed library held by the process (ctang's
 * `GTA_Language.library`). An execution takes a reference to the layer that
 * is current when it is created and keeps that one for its whole life:
 * replacing the layer later changes nothing for a running execution, and a
 * library is freed only when the last execution that took it is destroyed.
 *
 * Threads: both functions may be called from any thread, at any time.
 */

#ifndef GHOTI_IO_GLTANG_LANGUAGE_H
#define GHOTI_IO_GLTANG_LANGUAGE_H

#include <ghoti.io/lang-tang/macros.h>

#include <ghoti.io/lang-tang/core.h>
#include <ghoti.io/lang-tang/handle.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Makes `library` the language layer; executions created from now on
 *   search it.
 *
 * The library is sealed and retained; the host may release its own reference
 * at once. A second call replaces the first; NULL clears the layer.
 *
 * @param library The library, or NULL for none.
 * @return ::GLTANG_OK.
 */
GLTANG_API GLTANG_Result gltang_language_set_library(GLTANG_Library * library);

/**
 * @brief The language layer now, as a new reference the caller releases with
 *   ::gltang_library_release; NULL when there is none.
 */
GLTANG_API GLTANG_NO_DISCARD GLTANG_Library * gltang_language_library(void);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GLTANG_LANGUAGE_H
