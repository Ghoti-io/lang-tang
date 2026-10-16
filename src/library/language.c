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
 * @file
 *
 * The language layer: the process's one host-written library. See language.h.
 *
 * The slot is read and swapped under a spin flag held for a load and a retain
 * or a swap and nothing else, so a reader can never retain a library the
 * writer has already released.
 */

#include <ghoti.io/lang-tang/macros.h>

#include <stdatomic.h>

#include <ghoti.io/lang-tang/language.h>
#include "library_internal.h"

static atomic_flag slot_lock = ATOMIC_FLAG_INIT;
static GLTANG_Library * slot;

static void lock(void) {
  while (atomic_flag_test_and_set_explicit(&slot_lock, memory_order_acquire)) {
  }
}

static void unlock(void) {
  atomic_flag_clear_explicit(&slot_lock, memory_order_release);
}

GLTANG_Result gltang_language_set_library(GLTANG_Library * library) {
  GLTANG_Library * attached = gltang_library_attach(library);
  lock();
  GLTANG_Library * old = slot;
  slot = attached;
  unlock();
  gltang_library_release(old);
  return GLTANG_OK;
}

GLTANG_Library * gltang_language_library(void) {
  lock();
  GLTANG_Library * held = gltang_library_retain(slot);
  unlock();
  return held;
}
