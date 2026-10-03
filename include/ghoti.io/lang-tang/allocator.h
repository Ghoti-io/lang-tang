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
 * @file allocator.h
 * @stability stable
 *
 * Allocators for Ghoti.io Lang-tang.
 *
 * ::GLTANG_Allocator is cutil's `GCU_Allocator` under a local name. One
 * definition across the suite means an allocator written for any library works
 * with all of them. A `NULL` allocator argument means the default.
 *
 * This library allocates through the allocator returned by gltang_allocator(),
 * exactly as ctang did through its own, so that cutil's allocation counts stay
 * balanced. Charging parse memory to a runtime context is open for the story
 * that adds execution (see documentation/design.md).
 */

#ifndef GHOTI_IO_GLTANG_ALLOCATOR_H
#define GHOTI_IO_GLTANG_ALLOCATOR_H

#include <ghoti.io/lang-tang/macros.h>

#include <ghoti.io/cutil/allocator.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Allocator interface used by the library.
 *
 * All function pointers must be non-NULL. `calloc_fn` must treat overflow of
 * `nitems * size` as failure. A zero-size request returns a usable non-NULL
 * pointer, so NULL always means failure.
 */
typedef GCU_Allocator GLTANG_Allocator;

/**
 * @brief The process-global default allocator.
 *
 * @return cutil's default allocator. Never NULL.
 */
GLTANG_API const GLTANG_Allocator * gltang_allocator_default(void);

/**
 * @brief The allocator this library allocates through.
 *
 * Every buffer the library hands back is obtained with `gcu_malloc()` and must
 * be released with `gcu_free()`, so that the allocation counts cutil keeps stay
 * balanced. Those two are not interchangeable with libc's `malloc()` and
 * `free()`, and mixing them is invisible until a leak check reports a leak
 * that is not there.
 *
 * cutil's own containers default to gltang_allocator_default(), which is
 * stdlib-backed and therefore the wrong one for a container whose storage this
 * library will `gcu_free()`. Pass this allocator wherever a cutil container is
 * created for that purpose.
 *
 * @return A constant with static storage duration. Never NULL, never freed.
 */
GLTANG_API const GLTANG_Allocator * gltang_allocator(void);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GLTANG_ALLOCATOR_H */
