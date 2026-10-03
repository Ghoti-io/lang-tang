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
 * @file seeds.h
 * @stability stable
 *
 * The seed sequence: where a context's random generators get their seeds.
 *
 * ctang shared one `random.global` across the whole process and seeded
 * `random.default` from the clock. Here every execution has its own, and each
 * is seeded from a sequence the host makes once per group of contexts and hands
 * to each execution (::gltang_execution_set_seeds). A sequence holds one master
 * seed and an atomic counter. Draw `k` (counting from 0) is
 *
 *     splitmix64(master + k * 0x9E3779B97F4A7C15)
 *
 * in the standard splitmix64 construction (the state is advanced by the golden
 * gamma, then mixed), so any number of threads may draw from one sequence, and
 * two sequences made from one master seed give one series of seeds. No
 * generator is ever seeded from a clock.
 *
 * Two executions given sequences of the same master seed, and the same order of
 * first use of a generator, get the same random numbers.
 *
 * The sequence is reference counted: the host's reference is the one
 * ::gltang_seeds_create returns, and an execution that is handed the sequence
 * takes its own, so the host may release its own at once.
 *
 * Threads: ::gltang_seeds_next, ::gltang_seeds_retain and
 * ::gltang_seeds_destroy may be called from any thread.
 */

#ifndef GHOTI_IO_GLTANG_SEEDS_H
#define GHOTI_IO_GLTANG_SEEDS_H

#include <ghoti.io/lang-tang/macros.h>

#include <ghoti.io/lang-tang/core.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A seed sequence. Opaque. */
typedef struct GLTANG_SeedSequence GLTANG_SeedSequence;

/**
 * @brief Makes a sequence from a master seed the host chose.
 *
 * @param master The master seed.
 * @param out_seeds Receives the sequence, with one reference. Written only on
 *   success.
 * @return ::GLTANG_OK; ::GLTANG_ERR_OOM; ::GLTANG_ERR_INVALID for NULL.
 */
GLTANG_API GLTANG_Result gltang_seeds_create(
    uint64_t master, GLTANG_SeedSequence ** out_seeds);

/**
 * @brief Makes a sequence whose master seed is operating-system entropy
 *   (`getrandom`, then `/dev/urandom`; `rand_s` on Windows).
 *
 * @param out_seeds Receives the sequence, with one reference. Written only on
 *   success.
 * @return ::GLTANG_OK; ::GLTANG_ERR_OOM; ::GLTANG_ERR_IO when the system has no
 *   entropy to give; ::GLTANG_ERR_INVALID for NULL.
 */
GLTANG_API GLTANG_Result gltang_seeds_create_random(
    GLTANG_SeedSequence ** out_seeds);

/**
 * @brief Takes another reference.
 *
 * @param seeds The sequence, or NULL.
 * @return `seeds`.
 */
GLTANG_API GLTANG_SeedSequence * gltang_seeds_retain(
    GLTANG_SeedSequence * seeds);

/**
 * @brief Gives a reference back; the last one frees the sequence.
 *
 * @param seeds The sequence, or NULL (ignored).
 */
GLTANG_API void gltang_seeds_destroy(GLTANG_SeedSequence * seeds);

/**
 * @brief The next seed of the sequence.
 *
 * @param seeds The sequence.
 * @return The seed; 0 for NULL.
 */
GLTANG_API uint64_t gltang_seeds_next(GLTANG_SeedSequence * seeds);

/**
 * @brief The sequence's master seed.
 *
 * @param seeds The sequence.
 * @return The master seed; 0 for NULL.
 */
GLTANG_API uint64_t gltang_seeds_master(const GLTANG_SeedSequence * seeds);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GLTANG_SEEDS_H */
