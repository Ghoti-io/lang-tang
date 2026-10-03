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
 * The allocator this library allocates through, in the form cutil's generic
 * containers accept, and cutil's default.
 */

#include <ghoti.io/lang-tang/macros.h>

#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/cutil/memory.h>
#include <ghoti.io/cutil/safemath.h>
#include <ghoti.io/lang-tang/allocator.h>

/**
 * The contract these four honour is cutil's, not libc's, and it differs in
 * two places that a container built on the allocator is entitled to rely on:
 * a zero-size request yields a real pointer rather than a possible NULL, so
 * that NULL means failure and nothing else, and the product in a calloc() is
 * checked rather than assumed.  cutil's own default allocator states both; an
 * allocator that dropped them would be a subtly different one.
 *
 * They also keep one invariant that the default allocator does not need and
 * this library does: every live block has been counted exactly once as an
 * allocation, and is counted exactly once when it is released.  cutil's
 * counters do not maintain that by themselves - gcu_malloc() and gcu_calloc()
 * increment the allocation count, but gcu_realloc() increments nothing while
 * gcu_free() always increments, NULL included.  That is consistent for a
 * block that is malloc()ed, grown and freed, and wrong at both ends for the
 * way a cutil container actually allocates: GCU_Array obtains its storage by
 * calling realloc() against a NULL pointer and never calls malloc() at all,
 * so its buffer would be freed having never been counted, and gcu_array_
 * destroy() would then count a second free for the NULL data pointer that
 * gcu_array_steal() left behind.  Two spurious frees per array.
 *
 * The tests assert that allocations and frees balance around every program
 * run, so an allocator that let those through would report a leak that is not
 * there - or, worse, hide a real one behind the noise.
 */

static void * gltang_allocator_malloc(GLTANG_MAYBE_UNUSED(void * ctx), size_t size) {
  return gcu_malloc(size ? size : 1);
}


static void * gltang_allocator_calloc(GLTANG_MAYBE_UNUSED(void * ctx), size_t nitems, size_t size) {
  // calloc() is permitted to detect this itself, but is not required to on
  // every platform, so it is checked here rather than relied upon.
  size_t total;
  if (!gcu_safe_mul_size(nitems, size, &total)) {
    return NULL;
  }
  return gcu_calloc(1, total ? total : 1);
}


static void * gltang_allocator_realloc(void * ctx, void * ptr, size_t size) {
  // Growing a block that already exists changes nothing about how many blocks
  // are outstanding, so it is not counted - which is what gcu_realloc() does.
  // Reallocating from NULL is not a growth, though: it is this block's first
  // allocation, and the free that eventually matches it will be counted, so
  // this one has to be too.
  if (!ptr) {
    return gltang_allocator_malloc(ctx, size);
  }
  // Never shrink to nothing. glibc's realloc() releases the block and returns
  // NULL for a zero size, which a caller reads as failure and which would
  // leave the free uncounted.
  return gcu_realloc(ptr, size ? size : 1);
}


static void gltang_allocator_free(GLTANG_MAYBE_UNUSED(void * ctx), void * ptr) {
  // A NULL free is dropped rather than forwarded. gcu_free() counts the call
  // before it looks at the pointer, so a free(NULL) - which does nothing to
  // the heap - would still land in the free count with no allocation to match
  // it. A container reaching here with NULL is ordinary: gcu_array_destroy()
  // releases its data pointer even when gcu_array_steal() has already taken
  // it away.
  if (ptr) {
    gcu_free(ptr);
  }
}


static const GCU_Allocator allocator = {
  .ctx = NULL,
  .malloc_fn = gltang_allocator_malloc,
  .calloc_fn = gltang_allocator_calloc,
  .realloc_fn = gltang_allocator_realloc,
  .free_fn = gltang_allocator_free,
};


const GLTANG_Allocator * gltang_allocator(void) {
  return &allocator;
}


const GLTANG_Allocator * gltang_allocator_default(void) {
  return gcu_allocator_default();
}
