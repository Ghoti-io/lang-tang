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
 * The flat layout of a string, shared by the heap objects of the interpreter
 * and the constants of a program.
 *
 * One block holds everything, so a string is one heap object (no destroy hook,
 * charged to the budget by the allocator that served it) and a program constant
 * is one block that a run copies into the heap as it is:
 *
 *     header          kind, segment count, byte length, grapheme length
 *     segments        one 64-bit word each: (type << 32) | first grapheme
 *     offsets         (graphemes + 1) 32-bit byte offsets, padded to 8 bytes;
 *                     absent when every grapheme is one byte, which is the
 *                     common case and saves four bytes a character
 *     bytes           the UTF-8 text and a terminating NUL
 *
 * A grapheme is one byte exactly when the grapheme count equals the byte
 * count, so no flag is needed. Offsets are computed once, when the text is
 * made, and carried through concatenation and slicing without re-breaking
 * (ctang's behaviour, which design.md records).
 */

#ifndef GHOTI_IO_GLTANG_VM_STRING_LAYOUT_H
#define GHOTI_IO_GLTANG_VM_STRING_LAYOUT_H

#include <ghoti.io/lang-tang/macros.h>

#include <stddef.h>
#include <stdint.h>
#include <ghoti.io/lang-tang/unicodeString.h>

/** @brief The header of a flat string. */
typedef struct GLTANG_StringBlock {
  uint32_t kind;             ///< The heap object kind; unused in a constant.
  uint32_t segment_count;    ///< At least one.
  uint64_t byte_length;      ///< Not counting the terminator.
  uint64_t grapheme_length;  ///< Graphemes.
} GLTANG_StringBlock;

/** @brief The segment words. */
static inline uint64_t * gltang_string_segments(const GLTANG_StringBlock * s) {
  return (uint64_t *)(void *)((char *)s + sizeof(GLTANG_StringBlock));
}

/** @brief Whether the block carries an offsets table. */
static inline int gltang_string_has_offsets(const GLTANG_StringBlock * s) {
  return s->grapheme_length != s->byte_length;
}

/** @brief The offsets table, or NULL when every grapheme is one byte. */
static inline uint32_t * gltang_string_offsets(const GLTANG_StringBlock * s) {
  if (!gltang_string_has_offsets(s)) {
    return NULL;
  }
  return (uint32_t *)(void *)((char *)s + sizeof(GLTANG_StringBlock) + (size_t)s->segment_count * 8u);
}

/** @brief The bytes of the text. */
static inline char * gltang_string_bytes(const GLTANG_StringBlock * s) {
  size_t offsets_bytes = 0;
  if (gltang_string_has_offsets(s)) {
    offsets_bytes = (size_t)((s->grapheme_length + 1) * sizeof(uint32_t));
    offsets_bytes = (offsets_bytes + 7u) & ~(size_t)7u;
  }
  return (char *)s + sizeof(GLTANG_StringBlock) + (size_t)s->segment_count * 8u + offsets_bytes;
}

/** @brief The size of a block for these counts; 0 on overflow. */
static inline size_t gltang_string_block_size(size_t segments, size_t byte_length, size_t graphemes) {
  size_t size = sizeof(GLTANG_StringBlock);
  if (segments > (SIZE_MAX / 8u) - 4u) {
    return 0;
  }
  size += segments * 8u;
  if (graphemes != byte_length) {
    if (graphemes > (SIZE_MAX / 8u) - 4u) {
      return 0;
    }
    size_t offsets_bytes = (graphemes + 1) * sizeof(uint32_t);
    size += (offsets_bytes + 7u) & ~(size_t)7u;
  }
  if (byte_length > SIZE_MAX - size - 1u) {
    return 0;
  }
  return size + byte_length + 1u;
}

/** @brief The byte offset of grapheme `g`, for `g` up to the grapheme count. */
static inline size_t gltang_string_byte_offset(const GLTANG_StringBlock * s, size_t g) {
  const uint32_t * offsets = gltang_string_offsets(s);
  return offsets ? offsets[g] : g;
}

/** @brief The tag word of a segment. */
#define GLTANG_SEGMENT_WORD(type, first) ((((uint64_t)(type)) << 32) | (uint64_t)(first))
/** @brief The string type of a segment word. */
#define GLTANG_SEGMENT_TYPE(word) ((GLTANG_String_Type)((word) >> 32))
/** @brief The first grapheme of a segment word. */
#define GLTANG_SEGMENT_FIRST(word) ((size_t)((word) & 0xFFFFFFFFu))

#endif /* GHOTI_IO_GLTANG_VM_STRING_LAYOUT_H */
