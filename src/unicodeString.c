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
#include "vm/vm_internal.h"

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


/** The grapheme that holds byte `at`: the last whose start is not past it. */
static size_t grapheme_holding(const GLTANG_Unicode_String * string, size_t at) {
  size_t low = 0;
  size_t high = string->grapheme_length;
  while (low < high) {
    size_t mid = low + (high - low + 1u) / 2u;
    if (string->grapheme_offsets->data[mid].ui32 <= at) {
      low = mid;
    }
    else {
      high = mid - 1u;
    }
  }
  return low;
}

/** Appends the runs of `from` to `into`, whose bytes begin at `base` in `into`'s string. */
static bool append_runs(GCU_Vector64 * runs, const GLTANG_Unicode_String * into, const GLTANG_Unicode_String * from, size_t base) {
  if (!from->byte_length) {
    return true;
  }
  for (size_t i = 0; i < from->string_type->count; ++i) {
    GLTANG_String_Type type = (GLTANG_String_Type)GLTANG_UC_GET_TYPE_FROM_TYPE_OFFSET_PAIR(from->string_type->data[i]);
    size_t byte = from->grapheme_offsets->data[GLTANG_UC_GET_OFFSET_FROM_TYPE_OFFSET_PAIR(from->string_type->data[i])].ui32;
    size_t grapheme = grapheme_holding(into, base + byte);
    size_t last = runs->count ? GLTANG_UC_GET_OFFSET_FROM_TYPE_OFFSET_PAIR(runs->data[runs->count - 1u]) : 0;
    // A grapheme belongs to the run it began in; a run that starts inside a
    // grapheme the join made is absorbed, and equal tags are one run.
    if (runs->count && (grapheme <= last || (GLTANG_String_Type)GLTANG_UC_GET_TYPE_FROM_TYPE_OFFSET_PAIR(runs->data[runs->count - 1u]) == type)) {
      continue;
    }
    if (!gcu_vector64_append(runs, GLTANG_UC_MAKE_TYPE_OFFSET_PAIR(type, grapheme))) {
      return false;
    }
  }
  return true;
}

GLTANG_Unicode_String * gltang_unicode_string_concat(const GLTANG_Unicode_String * string1, const GLTANG_Unicode_String * string2) {
  if (!string1 || !string2) {
    return NULL;
  }
  size_t length;
  if (!gcu_safe_add_size(string1->byte_length, string2->byte_length, &length) || length == SIZE_MAX) {
    return NULL;
  }
  char * buffer = gcu_malloc(length + 1u);
  if (!buffer) {
    return NULL;
  }
  if (string1->byte_length) {
    memcpy(buffer, string1->buffer, string1->byte_length);
  }
  if (string2->byte_length) {
    memcpy(buffer + string1->byte_length, string2->buffer, string2->byte_length);
  }
  buffer[length] = '\0';
  GLTANG_Unicode_String * joined = gltang_unicode_string_create_and_adopt(buffer, length, GLTANG_UNICODE_STRING_TYPE_TRUSTED);
  if (!joined) {
    gcu_free(buffer);
    return NULL;
  }
  // The runs: those of each non-empty input, put on the joined graphemes. Two
  // empty strings join into an empty string of the second's tag.
  joined->string_type->count = 0;
  if (!append_runs(joined->string_type, joined, string1, 0) || !append_runs(joined->string_type, joined, string2, string1->byte_length)) {
    gltang_unicode_string_destroy(joined);
    return NULL;
  }
  if (!joined->string_type->count) {
    GLTANG_String_Type type = (GLTANG_String_Type)GLTANG_UC_GET_TYPE_FROM_TYPE_OFFSET_PAIR(string2->string_type->data[0]);
    if (!gcu_vector64_append(joined->string_type, GLTANG_UC_MAKE_TYPE_OFFSET_PAIR(type, 0))) {
      gltang_unicode_string_destroy(joined);
      return NULL;
    }
  }
  return joined;
}

GLTANG_Unicode_String * gltang_unicode_string_substring(const GLTANG_Unicode_String * string, size_t grapheme_start, size_t grapheme_count) {
  if (!string) {
    return NULL;
  }
  if (grapheme_start >= string->grapheme_length || grapheme_count == 0) {
    return gltang_unicode_string_create("", 0, GLTANG_UNICODE_STRING_TYPE_TRUSTED);
  }
  size_t end = grapheme_count >= string->grapheme_length - grapheme_start ? string->grapheme_length : grapheme_start + grapheme_count;
  size_t byte_start = string->grapheme_offsets->data[grapheme_start].ui32;
  size_t byte_end = string->grapheme_offsets->data[end].ui32;
  // The run that holds the first grapheme: the last that begins at or before it.
  size_t first = 0;
  while (first + 1u < string->string_type->count && GLTANG_UC_GET_OFFSET_FROM_TYPE_OFFSET_PAIR(string->string_type->data[first + 1u]) <= grapheme_start) {
    ++first;
  }
  GLTANG_Unicode_String * taken = gltang_unicode_string_create(string->buffer + byte_start, byte_end - byte_start,
    (GLTANG_String_Type)GLTANG_UC_GET_TYPE_FROM_TYPE_OFFSET_PAIR(string->string_type->data[first]));
  if (!taken) {
    return NULL;
  }
  // Every later run that begins before the end; a run beginning at the end is
  // the text after the substring.
  for (size_t i = first + 1u; i < string->string_type->count; ++i) {
    size_t at = GLTANG_UC_GET_OFFSET_FROM_TYPE_OFFSET_PAIR(string->string_type->data[i]);
    if (at >= end) {
      break;
    }
    if (!gcu_vector64_append(taken->string_type, GLTANG_UC_MAKE_TYPE_OFFSET_PAIR(GLTANG_UC_GET_TYPE_FROM_TYPE_OFFSET_PAIR(string->string_type->data[i]), at - grapheme_start))) {
      gltang_unicode_string_destroy(taken);
      return NULL;
    }
  }
  return taken;
}

GLTANG_Unicode_Rendered_String gltang_unicode_string_render(const GLTANG_Unicode_String * string) {
  GLTANG_Unicode_Rendered_String rendered = {NULL, 0};
  if (!string || !string->buffer) {
    return rendered;
  }
  size_t count = string->string_type->count;
  GLTANG_OutputSegment * segments = gcu_malloc((count ? count : 1u) * sizeof(*segments));
  if (!segments) {
    return rendered;
  }
  for (size_t i = 0; i < count; ++i) {
    segments[i].offset = string->grapheme_offsets->data[GLTANG_UC_GET_OFFSET_FROM_TYPE_OFFSET_PAIR(string->string_type->data[i])].ui32;
    segments[i].type = (GLTANG_String_Type)GLTANG_UC_GET_TYPE_FROM_TYPE_OFFSET_PAIR(string->string_type->data[i]);
  }
  if (!gltang_vm_render_segments(string->buffer, string->byte_length, segments, count, &rendered.buffer, &rendered.length)) {
    rendered.buffer = NULL;
    rendered.length = 0;
  }
  gcu_free(segments);
  return rendered;
}

GLTANG_Unicode_Rendered_String gltang_unicode_string_html_encode(const char * source, size_t length) {
  GLTANG_Unicode_Rendered_String rendered = {NULL, 0};
  if (!source && length) {
    return rendered;
  }
  GLTANG_OutputSegment segment = {.offset = 0, .type = GLTANG_UNICODE_STRING_TYPE_HTML};
  if (!gltang_vm_render_segments(source ? source : "", length, &segment, 1u, &rendered.buffer, &rendered.length)) {
    rendered.buffer = NULL;
    rendered.length = 0;
  }
  return rendered;
}
