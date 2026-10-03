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
 * The seed sequence: a master seed and an atomic draw counter, mixed with the
 * standard splitmix64 construction. See seeds.h for the contract.
 */

/* Before any include: -std=c17 hides getrandom and open unless a feature macro
 * is set, and a feature macro after the first include is ignored. */
#if defined(_WIN32)
#define _CRT_RAND_S
#elif !defined(__APPLE__)
#define _GNU_SOURCE
#endif

#include <ghoti.io/lang-tang/macros.h>

#include <stdatomic.h>
#include <errno.h>
#include <stdlib.h>
#include <ghoti.io/cutil/memory.h>
#include <ghoti.io/lang-tang/seeds.h>
#include "library_internal.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/random.h>
#endif
#endif

#define GOLDEN_GAMMA UINT64_C(0x9E3779B97F4A7C15)

struct GLTANG_SeedSequence {
  atomic_size_t references;
  uint64_t master;
  atomic_uint_fast64_t counter;
};

uint64_t gltang_splitmix64_mix(uint64_t state) {
  // The standard construction: advance by the golden gamma, then mix.
  uint64_t z = state + GOLDEN_GAMMA;
  z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
  z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
  return z ^ (z >> 31);
}

bool gltang_entropy(void * bytes, size_t length) {
#if defined(_WIN32)
  unsigned char * out = bytes;
  for (size_t i = 0; i < length;) {
    unsigned int word;
    if (rand_s(&word) != 0) {
      return false;
    }
    for (size_t b = 0; b < sizeof(word) && i < length; ++b, ++i) {
      out[i] = (unsigned char)(word >> (8u * b));
    }
  }
  return true;
#else
  unsigned char * out = bytes;
  size_t done = 0;
#if defined(__linux__)
  while (done < length) {
    ssize_t n = getrandom(out + done, length - done, 0);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    done += (size_t)n;
  }
  if (done == length) {
    return true;
  }
#endif
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0) {
    return false;
  }
  while (done < length) {
    ssize_t n = read(fd, out + done, length - done);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      close(fd);
      return false;
    }
    done += (size_t)n;
  }
  close(fd);
  return true;
#endif
}

GLTANG_Result gltang_seeds_create(uint64_t master, GLTANG_SeedSequence ** out_seeds) {
  if (!out_seeds) {
    return GLTANG_ERR_INVALID;
  }
  GLTANG_SeedSequence * seeds = gcu_malloc(sizeof(GLTANG_SeedSequence));
  if (!seeds) {
    return GLTANG_ERR_OOM;
  }
  atomic_init(&seeds->references, 1);
  atomic_init(&seeds->counter, 0);
  seeds->master = master;
  *out_seeds = seeds;
  return GLTANG_OK;
}

GLTANG_Result gltang_seeds_create_random(GLTANG_SeedSequence ** out_seeds) {
  if (!out_seeds) {
    return GLTANG_ERR_INVALID;
  }
  uint64_t master;
  if (!gltang_entropy(&master, sizeof(master))) {
    return GLTANG_ERR_IO;
  }
  return gltang_seeds_create(master, out_seeds);
}

GLTANG_SeedSequence * gltang_seeds_retain(GLTANG_SeedSequence * seeds) {
  if (seeds) {
    atomic_fetch_add_explicit(&seeds->references, 1, memory_order_relaxed);
  }
  return seeds;
}

void gltang_seeds_destroy(GLTANG_SeedSequence * seeds) {
  if (!seeds) {
    return;
  }
  if (atomic_fetch_sub_explicit(&seeds->references, 1, memory_order_acq_rel) == 1) {
    gcu_free(seeds);
  }
}

uint64_t gltang_seeds_next(GLTANG_SeedSequence * seeds) {
  if (!seeds) {
    return 0;
  }
  uint64_t k = atomic_fetch_add_explicit(&seeds->counter, 1, memory_order_relaxed);
  // splitmix64(master + k * gamma): the mix advances the state once more, so
  // draw k is the (k+1)th output of the generator started at `master`.
  return gltang_splitmix64_mix(seeds->master + k * GOLDEN_GAMMA);
}

uint64_t gltang_seeds_master(const GLTANG_SeedSequence * seeds) {
  return seeds ? seeds->master : 0;
}
