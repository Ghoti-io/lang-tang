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

#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <ghoti.io/cutil/array.h>
#include <ghoti.io/cutil/memory.h>
#include <ghoti.io/cutil/safemath.h>
#include <ghoti.io/unicode/break.h>
#include <ghoti.io/unicode/utf.h>
#include <ghoti.io/lang-tang/macros.h>
#include <ghoti.io/lang-tang/unicodeString.h>

static bool gltang_unicode_string_get_grapheme_offsets(GCU_Vector32 * grapheme_offsets, const char * buffer, size_t length) {
  assert(grapheme_offsets);
  assert(buffer);

  // An empty string is one grapheme boundary at offset 0. The break iterator
  // reports no boundary on empty text, because there is nothing to fall between.
  if (!length) {
    return gcu_vector32_append(grapheme_offsets, GCU_TYPE32_UI32(0));
  }

  // Ill-formed UTF-8 used to fail the conversion into the break iterator's
  // encoding, and string creation failed with it. Refuse here for the same
  // reason: a boundary inside a bad sequence is not a grapheme.
  if (guni_utf8_validate(buffer, length, NULL) != GUNI_OK) {
    return false;
  }

  // The offsets are stored as uint32. A boundary past that cannot be recorded.
  if (length > UINT32_MAX) {
    return false;
  }

  // Worst case: every byte is its own grapheme, plus the sentinel at the end.
  // The vector may already hold entries, so the reserve covers those too.
  size_t cap = 0;
  size_t needed = 0;
  if (!gcu_safe_add_size(length, 1, &cap)
      || !gcu_safe_add_size(grapheme_offsets->count, cap, &needed)
      || !gcu_vector32_reserve(grapheme_offsets, needed)) {
    return false;
  }

  // guni_break_all() writes size_t. The vector's slots are uint32, so the
  // table is filled into a temporary buffer and then copied across.
  size_t bytes = 0;
  if (!gcu_safe_mul_size(cap, sizeof(size_t), &bytes)) {
    return false;
  }
  size_t * bounds = gcu_malloc(bytes);
  if (bounds == NULL) {
    return false;
  }
  size_t count = 0;
  GUNI_Result result = guni_break_all(NULL, buffer, length, bounds, cap, &count);
  if (result != GUNI_OK || count < 2 || bounds[count - 1] != length) {
    gcu_free(bounds);
    return false;
  }

  size_t base = grapheme_offsets->count;
  for (size_t i = 0; i < count; i++) {
    grapheme_offsets->data[base + i] = GCU_TYPE32_UI32((uint32_t)bounds[i]);
  }
  grapheme_offsets->count = base + count;
  gcu_free(bounds);
  return true;
}


GLTANG_Unicode_String * gltang_unicode_string_create(const char * source, size_t length, GLTANG_String_Type type) {
  assert(source);

  char * buffer = gcu_malloc(length + 1);
  if (!buffer) {
    return NULL;
  }
  if (length) {
    memcpy(buffer, source, length);
  }
  buffer[length] = '\0';
  GLTANG_Unicode_String * string = gltang_unicode_string_create_and_adopt(buffer, length, type);
  if (!string) {
    gcu_free(buffer);
  }
  return string;
}


GLTANG_Unicode_String * gltang_unicode_string_create_and_adopt(const char * source, size_t length, GLTANG_String_Type type) {
  assert(source);

  // Allocate space for the string.
  GLTANG_Unicode_String * string = gcu_calloc(sizeof(GLTANG_Unicode_String), 1);
  if (string == NULL) {
    return NULL;
  }

  // Adopt the buffer.
  string->buffer = source;
  string->byte_length = length;

  // Create the grapheme offsets.
  // For ease of use, we will add an extra offset at the end of the string
  // that points to the end of the string.
  // If the string only contains ASCII characters, then the grapheme offsets
  // will be the same as the byte offsets and will require (length + 1)
  // entries as a worst case.  If the string contains non-ASCII characters,
  // then the grapheme offsets will be different from the byte offsets and
  // will require fewer than (length + 1) entries, so we will allocate the
  // worst-case number of entries to ensure that no allocation failures can
  // happen later.
  string->grapheme_offsets = gcu_vector32_create(length + 1);
  if (string->grapheme_offsets == NULL) {
    gcu_free(string);
    return NULL;
  }
  if (!gltang_unicode_string_get_grapheme_offsets(string->grapheme_offsets, source, length)) {
    gcu_vector32_destroy(string->grapheme_offsets);
    gcu_free(string);
    return NULL;
  }
  string->grapheme_length = gcu_vector32_count(string->grapheme_offsets) - 1;

  // Create the string type vector.
  // It will only contain one type, so we will allocate one entry.
  string->string_type = gcu_vector64_create(1);
  if (string->string_type == NULL) {
    gcu_vector32_destroy(string->grapheme_offsets);
    gcu_free(string);
    return NULL;
  }
  gcu_vector64_append(string->string_type, GLTANG_UC_MAKE_TYPE_OFFSET_PAIR(type, 0));

  return string;
}


void gltang_unicode_string_destroy(GLTANG_Unicode_String * string) {
  assert(string);

  gcu_vector32_destroy(string->grapheme_offsets);
  gcu_vector64_destroy(string->string_type);
  gcu_free((void *)string->buffer);
  gcu_free(string);
}
