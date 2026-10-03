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
 * Strings: grapheme sequences with segment-tagged encodings, as flat heap
 * objects (string_layout.h).
 *
 * The behaviour is ctang's, written for this layout: concatenation joins the
 * segment lists and the grapheme tables without re-breaking, a substring keeps
 * the segments it covers, and rendering encodes every segment by its tag.
 * Every copy a program can make large is paced: the bytes go over in chunks,
 * with a runtime poll between them (AD-21), and the half-built string is a
 * temporary root while it waits at a poll.
 */

#include <ghoti.io/lang-tang/macros.h>

#include <stdlib.h>
#include <string.h>
#include <ghoti.io/cutil/memory.h>
#include <ghoti.io/lang-tang/unicodeString.h>
#include <ghoti.io/unicode/break.h>
#include <ghoti.io/unicode/utf.h>
#include "vm_internal.h"

// ---------------------------------------------------------------------------
// Pacing
// ---------------------------------------------------------------------------

typedef struct Pacer {
  GLTANG_Execution * exec;
  size_t since;  ///< Bytes (or element-equivalents) since the last poll.
} Pacer;

static GLTANG_Status pace(Pacer * pacer, size_t units) {
  pacer->since += units;
  if (pacer->since >= GLTANG_POLL_BYTES) {
    size_t work = pacer->since / GLTANG_WORK_BYTES_PER_FUEL;
    pacer->since = 0;
    return gltang_vm_native_poll(pacer->exec, work ? work : 1);
  }
  return GLTANG_ST_OK;
}

// ---------------------------------------------------------------------------
// Creating
// ---------------------------------------------------------------------------

GLTANG_Status gltang_vm_string_alloc(GLTANG_Execution * exec, size_t segments, size_t bytes, size_t graphemes, GLTANG_StringBlock ** out) {
  size_t size = gltang_string_block_size(segments, bytes, graphemes);
  // Offsets are 32-bit: a string past 4 GiB cannot record where its graphemes
  // start. It is refused as an allocation failure would be.
  if (!size || bytes > UINT32_MAX) {
    return GLTANG_ST_OOM;
  }
  void * object;
  GLTANG_Status st = gltang_vm_alloc(exec, &gltang_type_string, size, &object);
  if (st != GLTANG_ST_OK) {
    return st;
  }
  GLTANG_StringBlock * s = object;
  s->kind = GLTANG_OBJ_STRING;
  s->segment_count = (uint32_t)segments;
  s->byte_length = bytes;
  s->grapheme_length = graphemes;
  *out = s;
  return GLTANG_ST_OK;
}

GLTANG_Value gltang_vm_string_from_ascii(GLTANG_Execution * exec, const char * bytes, size_t length, GLTANG_String_Type type) {
  GLTANG_StringBlock * s;
  GLTANG_Status st = gltang_vm_string_alloc(exec, 1, length, length, &s);
  if (st != GLTANG_ST_OK) {
    return gltang_vm_failure_value(exec, st);
  }
  gltang_string_segments(s)[0] = GLTANG_SEGMENT_WORD(type, 0);
  if (length) {
    memcpy(gltang_string_bytes(s), bytes, length);
  }
  return gltang_value_of(s);
}

GLTANG_Value gltang_vm_string_from_block(GLTANG_Execution * exec, const GLTANG_StringBlock * block, size_t size) {
  void * object;
  GLTANG_Status st = gltang_vm_alloc(exec, &gltang_type_string, size, &object);
  if (st != GLTANG_ST_OK) {
    return gltang_vm_failure_value(exec, st);
  }
  memcpy(object, block, size);
  GLTANG_StringBlock * s = object;
  s->kind = GLTANG_OBJ_STRING;
  return gltang_value_of(s);
}

static bool is_plain_ascii(const char * bytes, size_t length) {
  // A carriage return may be half of a CR LF grapheme, so text that has one
  // takes the general path.
  for (size_t i = 0; i < length; ++i) {
    unsigned char c = (unsigned char)bytes[i];
    if (c >= 0x80 || c == '\r') {
      return false;
    }
  }
  return true;
}

GLTANG_Value gltang_vm_string_from_utf8(GLTANG_Execution * exec, const char * bytes, size_t length, GLTANG_String_Type type) {
  if (is_plain_ascii(bytes, length)) {
    return gltang_vm_string_from_ascii(exec, bytes, length, type);
  }
  if (length > UINT32_MAX || guni_utf8_validate(bytes, length, NULL) != GUNI_OK) {
    return GLTANG_V_NULL;
  }
  // The break iterator is one library call and cannot be paced from inside, so
  // its cost is charged before it.
  if (gltang_vm_native_poll(exec, length / GLTANG_WORK_BYTES_PER_FUEL + 1) != GLTANG_ST_OK) {
    return GLTANG_V_UNWIND;
  }
  size_t * bounds = gcu_allocator_malloc(exec->allocator, (length + 1u) * sizeof(size_t));
  if (!bounds) {
    return exec->roots[GLTANG_ROOT_OOM];
  }
  size_t count = 0;
  GUNI_Result r = guni_break_all(NULL, bytes, length, bounds, length + 1u, &count);
  if (r != GUNI_OK || count < 2 || bounds[count - 1u] != length) {
    gcu_allocator_free(exec->allocator, bounds);
    return GLTANG_V_NULL;
  }
  size_t graphemes = count - 1u;
  GLTANG_StringBlock * s;
  GLTANG_Status st = gltang_vm_string_alloc(exec, 1, length, graphemes, &s);
  if (st != GLTANG_ST_OK) {
    gcu_allocator_free(exec->allocator, bounds);
    return gltang_vm_failure_value(exec, st);
  }
  gltang_string_segments(s)[0] = GLTANG_SEGMENT_WORD(type, 0);
  uint32_t * offsets = gltang_string_offsets(s);
  if (offsets) {
    for (size_t i = 0; i <= graphemes; ++i) {
      offsets[i] = (uint32_t)bounds[i];
    }
  }
  memcpy(gltang_string_bytes(s), bytes, length);
  gcu_allocator_free(exec->allocator, bounds);
  return gltang_value_of(s);
}

// ---------------------------------------------------------------------------
// Looking
// ---------------------------------------------------------------------------

static GLTANG_String_Type first_type(const GLTANG_StringBlock * s) {
  return GLTANG_SEGMENT_TYPE(gltang_string_segments(s)[0]);
}

static GLTANG_String_Type last_type(const GLTANG_StringBlock * s) {
  return GLTANG_SEGMENT_TYPE(gltang_string_segments(s)[s->segment_count - 1u]);
}

/** The index of the segment holding grapheme `g`. */
static size_t segment_of(const GLTANG_StringBlock * s, size_t g) {
  const uint64_t * segments = gltang_string_segments(s);
  size_t low = 0;
  size_t high = s->segment_count;
  while (low + 1 < high) {
    size_t middle = low + (high - low) / 2;
    if (GLTANG_SEGMENT_FIRST(segments[middle]) <= g) {
      low = middle;
    }
    else {
      high = middle;
    }
  }
  return low;
}

bool gltang_vm_string_equal(GLTANG_Value a, GLTANG_Value b) {
  const GLTANG_StringBlock * x = gltang_vm_string(a);
  const GLTANG_StringBlock * y = gltang_vm_string(b);
  return x->byte_length == y->byte_length && !memcmp(gltang_string_bytes(x), gltang_string_bytes(y), (size_t)x->byte_length);
}

int gltang_vm_string_compare(GLTANG_Value a, GLTANG_Value b) {
  const GLTANG_StringBlock * x = gltang_vm_string(a);
  const GLTANG_StringBlock * y = gltang_vm_string(b);
  size_t common = (size_t)(x->byte_length < y->byte_length ? x->byte_length : y->byte_length);
  int c = common ? memcmp(gltang_string_bytes(x), gltang_string_bytes(y), common) : 0;
  if (c) {
    return c < 0 ? -1 : 1;
  }
  return x->byte_length < y->byte_length ? -1 : (x->byte_length > y->byte_length ? 1 : 0);
}

uint64_t gltang_vm_string_hash(const char * bytes, size_t length) {
  uint64_t hash = 1469598103934665603ull;
  for (size_t i = 0; i < length; ++i) {
    hash = (hash ^ (unsigned char)bytes[i]) * 1099511628211ull;
  }
  return hash;
}

// ---------------------------------------------------------------------------
// Concatenation
// ---------------------------------------------------------------------------

GLTANG_Value gltang_vm_string_concat(GLTANG_Execution * exec, GLTANG_Value a, GLTANG_Value b) {
  const GLTANG_StringBlock * x = gltang_vm_string(a);
  const GLTANG_StringBlock * y = gltang_vm_string(b);
  if (y->byte_length == 0) {
    return a;
  }
  if (x->byte_length == 0) {
    return b;
  }
  if (x->byte_length + y->byte_length > UINT32_MAX) {
    return exec->roots[GLTANG_ROOT_OOM];
  }
  bool merge = last_type(x) == first_type(y);
  size_t segments = (size_t)x->segment_count + y->segment_count - (merge ? 1u : 0u);
  size_t graphemes = (size_t)(x->grapheme_length + y->grapheme_length);
  size_t bytes = (size_t)(x->byte_length + y->byte_length);
  GLTANG_StringBlock * s;
  GLTANG_Status st = gltang_vm_string_alloc(exec, segments, bytes, graphemes, &s);
  if (st != GLTANG_ST_OK) {
    return gltang_vm_failure_value(exec, st);
  }
  uint64_t * out = gltang_string_segments(s);
  memcpy(out, gltang_string_segments(x), (size_t)x->segment_count * 8u);
  const uint64_t * ys = gltang_string_segments(y);
  for (size_t i = merge ? 1u : 0u; i < y->segment_count; ++i) {
    out[x->segment_count + i - (merge ? 1u : 0u)] = ys[i] + (uint64_t)x->grapheme_length;
  }
  GLTANG_Value result = gltang_value_of(s);
  // From here the string can wait at a poll, so it is kept alive.
  if (!gltang_vm_temp_push(exec, result)) {
    return exec->roots[GLTANG_ROOT_OOM];
  }
  Pacer pacer = {exec, 0};
  GLTANG_Status poll = GLTANG_ST_OK;
  uint32_t * offsets = gltang_string_offsets(s);
  if (offsets) {
    const uint32_t * xo = gltang_string_offsets(x);
    const uint32_t * yo = gltang_string_offsets(y);
    size_t xg = (size_t)x->grapheme_length;
    size_t yg = (size_t)y->grapheme_length;
    for (size_t i = 0; i < xg && poll == GLTANG_ST_OK; ++i) {
      offsets[i] = xo ? xo[i] : (uint32_t)i;
      if ((i & 1023u) == 1023u) {
        poll = pace(&pacer, 1024u * 4u);
      }
    }
    for (size_t i = 0; i <= yg && poll == GLTANG_ST_OK; ++i) {
      offsets[xg + i] = (uint32_t)(x->byte_length + (yo ? yo[i] : i));
      if ((i & 1023u) == 1023u) {
        poll = pace(&pacer, 1024u * 4u);
      }
    }
  }
  char * dest = gltang_string_bytes(s);
  const char * from_x = gltang_string_bytes(x);
  const char * from_y = gltang_string_bytes(y);
  for (size_t done = 0; done < x->byte_length && poll == GLTANG_ST_OK;) {
    size_t n = (size_t)x->byte_length - done;
    if (n > GLTANG_POLL_BYTES) {
      n = GLTANG_POLL_BYTES;
    }
    memcpy(dest + done, from_x + done, n);
    done += n;
    poll = pace(&pacer, n);
  }
  for (size_t done = 0; done < y->byte_length && poll == GLTANG_ST_OK;) {
    size_t n = (size_t)y->byte_length - done;
    if (n > GLTANG_POLL_BYTES) {
      n = GLTANG_POLL_BYTES;
    }
    memcpy(dest + x->byte_length + done, from_y + done, n);
    done += n;
    poll = pace(&pacer, n);
  }
  gltang_vm_temp_pop(exec);
  if (poll != GLTANG_ST_OK) {
    return GLTANG_V_UNWIND;
  }
  return result;
}

// ---------------------------------------------------------------------------
// Substrings and slices
// ---------------------------------------------------------------------------

static GLTANG_Value empty_string(GLTANG_Execution * exec) {
  return gltang_vm_string_from_ascii(exec, "", 0, GLTANG_UNICODE_STRING_TYPE_TRUSTED);
}

/** A run of graphemes [start, start + count) with its segments, `count` >= 1. */
static GLTANG_Value substring(GLTANG_Execution * exec, GLTANG_Value v, size_t start, size_t count) {
  const GLTANG_StringBlock * x = gltang_vm_string(v);
  size_t end = start + count;
  size_t first = segment_of(x, start);
  size_t last = segment_of(x, end - 1u);
  size_t segments = last - first + 1u;
  size_t byte_start = gltang_string_byte_offset(x, start);
  size_t byte_end = gltang_string_byte_offset(x, end);
  size_t bytes = byte_end - byte_start;
  GLTANG_StringBlock * s;
  GLTANG_Status st = gltang_vm_string_alloc(exec, segments, bytes, count, &s);
  if (st != GLTANG_ST_OK) {
    return gltang_vm_failure_value(exec, st);
  }
  uint64_t * out = gltang_string_segments(s);
  const uint64_t * in = gltang_string_segments(x);
  for (size_t i = 0; i < segments; ++i) {
    size_t from = GLTANG_SEGMENT_FIRST(in[first + i]);
    out[i] = GLTANG_SEGMENT_WORD(GLTANG_SEGMENT_TYPE(in[first + i]), i ? from - start : 0);
  }
  GLTANG_Value result = gltang_value_of(s);
  if (!gltang_vm_temp_push(exec, result)) {
    return exec->roots[GLTANG_ROOT_OOM];
  }
  Pacer pacer = {exec, 0};
  GLTANG_Status poll = GLTANG_ST_OK;
  uint32_t * offsets = gltang_string_offsets(s);
  const uint32_t * source_offsets = gltang_string_offsets(x);
  if (offsets) {
    for (size_t i = 0; i <= count && poll == GLTANG_ST_OK; ++i) {
      offsets[i] = (uint32_t)((source_offsets ? source_offsets[start + i] : start + i) - byte_start);
      if ((i & 1023u) == 1023u) {
        poll = pace(&pacer, 1024u * 4u);
      }
    }
  }
  char * dest = gltang_string_bytes(s);
  const char * from = gltang_string_bytes(x) + byte_start;
  for (size_t done = 0; done < bytes && poll == GLTANG_ST_OK;) {
    size_t n = bytes - done;
    if (n > GLTANG_POLL_BYTES) {
      n = GLTANG_POLL_BYTES;
    }
    memcpy(dest + done, from + done, n);
    done += n;
    poll = pace(&pacer, n);
  }
  gltang_vm_temp_pop(exec);
  if (poll != GLTANG_ST_OK) {
    return GLTANG_V_UNWIND;
  }
  return result;
}

GLTANG_Value gltang_vm_string_grapheme(GLTANG_Execution * exec, GLTANG_Value v, size_t index) {
  const GLTANG_StringBlock * x = gltang_vm_string(v);
  if (index >= x->grapheme_length) {
    return empty_string(exec);
  }
  return substring(exec, v, index, 1);
}

GLTANG_Value gltang_vm_string_slice(GLTANG_Execution * exec, GLTANG_Value v, int64_t start, int64_t count, int64_t step) {
  if (count <= 0) {
    return empty_string(exec);
  }
  if (step == 1) {
    return substring(exec, v, (size_t)start, (size_t)count);
  }
  const GLTANG_StringBlock * x = gltang_vm_string(v);
  // Pass one: how many bytes and how many segments the selection needs.
  size_t total = 0;
  size_t segments = 0;
  GLTANG_String_Type previous = GLTANG_UNICODE_STRING_TYPE_TRUSTED;
  Pacer pacer = {exec, 0};
  int64_t g = start;
  for (int64_t k = 0; k < count; ++k) {
    size_t gi = (size_t)g;
    total += gltang_string_byte_offset(x, gi + 1u) - gltang_string_byte_offset(x, gi);
    GLTANG_String_Type t = GLTANG_SEGMENT_TYPE(gltang_string_segments(x)[segment_of(x, gi)]);
    if (k == 0 || t != previous) {
      ++segments;
    }
    previous = t;
    if (k + 1 < count) {
      g += step;
    }
    if (pace(&pacer, 8u) != GLTANG_ST_OK) {
      return GLTANG_V_UNWIND;
    }
  }
  GLTANG_StringBlock * s;
  GLTANG_Status st = gltang_vm_string_alloc(exec, segments, total, (size_t)count, &s);
  if (st != GLTANG_ST_OK) {
    return gltang_vm_failure_value(exec, st);
  }
  GLTANG_Value result = gltang_value_of(s);
  if (!gltang_vm_temp_push(exec, result)) {
    return exec->roots[GLTANG_ROOT_OOM];
  }
  uint64_t * out = gltang_string_segments(s);
  uint32_t * offsets = gltang_string_offsets(s);
  char * dest = gltang_string_bytes(s);
  size_t written = 0;
  size_t segment = 0;
  GLTANG_Status poll = GLTANG_ST_OK;
  g = start;
  for (int64_t k = 0; k < count && poll == GLTANG_ST_OK; ++k) {
    size_t gi = (size_t)g;
    size_t from = gltang_string_byte_offset(x, gi);
    size_t to = gltang_string_byte_offset(x, gi + 1u);
    GLTANG_String_Type t = GLTANG_SEGMENT_TYPE(gltang_string_segments(x)[segment_of(x, gi)]);
    if (k == 0 || t != previous) {
      out[segment++] = GLTANG_SEGMENT_WORD(t, (uint64_t)k);
    }
    previous = t;
    if (offsets) {
      offsets[k] = (uint32_t)written;
    }
    memcpy(dest + written, gltang_string_bytes(x) + from, to - from);
    written += to - from;
    if (k + 1 < count) {
      g += step;
    }
    poll = pace(&pacer, 8u + (to - from));
  }
  if (offsets) {
    offsets[count] = (uint32_t)written;
  }
  gltang_vm_temp_pop(exec);
  if (poll != GLTANG_ST_OK) {
    return GLTANG_V_UNWIND;
  }
  return result;
}

// ---------------------------------------------------------------------------
// Retagging and rendering
// ---------------------------------------------------------------------------

GLTANG_Value gltang_vm_string_retag(GLTANG_Execution * exec, GLTANG_Value v, GLTANG_String_Type type) {
  const GLTANG_StringBlock * x = gltang_vm_string(v);
  if (x->segment_count == 1 && first_type(x) == type) {
    return v;
  }
  GLTANG_StringBlock * s;
  GLTANG_Status st = gltang_vm_string_alloc(exec, 1, (size_t)x->byte_length, (size_t)x->grapheme_length, &s);
  if (st != GLTANG_ST_OK) {
    return gltang_vm_failure_value(exec, st);
  }
  gltang_string_segments(s)[0] = GLTANG_SEGMENT_WORD(type, 0);
  GLTANG_Value result = gltang_value_of(s);
  if (!gltang_vm_temp_push(exec, result)) {
    return exec->roots[GLTANG_ROOT_OOM];
  }
  Pacer pacer = {exec, 0};
  GLTANG_Status poll = GLTANG_ST_OK;
  uint32_t * offsets = gltang_string_offsets(s);
  if (offsets) {
    const uint32_t * source = gltang_string_offsets(x);
    for (size_t i = 0; i <= x->grapheme_length && poll == GLTANG_ST_OK; ++i) {
      offsets[i] = source[i];
      if ((i & 1023u) == 1023u) {
        poll = pace(&pacer, 1024u * 4u);
      }
    }
  }
  char * dest = gltang_string_bytes(s);
  const char * from = gltang_string_bytes(x);
  for (size_t done = 0; done < x->byte_length && poll == GLTANG_ST_OK;) {
    size_t n = (size_t)x->byte_length - done;
    if (n > GLTANG_POLL_BYTES) {
      n = GLTANG_POLL_BYTES;
    }
    memcpy(dest + done, from + done, n);
    done += n;
    poll = pace(&pacer, n);
  }
  gltang_vm_temp_pop(exec);
  if (poll != GLTANG_ST_OK) {
    return GLTANG_V_UNWIND;
  }
  return result;
}

/** How a byte is encoded in a segment of this type: the bytes, and how many. */
static size_t encode_byte(GLTANG_String_Type type, unsigned char c, char * out) {
  switch (type) {
    case GLTANG_UNICODE_STRING_TYPE_TRUSTED:
      out[0] = (char)c;
      return 1;
    case GLTANG_UNICODE_STRING_TYPE_HTML:
    case GLTANG_UNICODE_STRING_TYPE_HTML_ATTRIBUTE:
      switch (c) {
        case '<': memcpy(out, "&lt;", 4); return 4;
        case '>': memcpy(out, "&gt;", 4); return 4;
        case '&': memcpy(out, "&amp;", 5); return 5;
        case '"':
          if (type == GLTANG_UNICODE_STRING_TYPE_HTML_ATTRIBUTE) {
            memcpy(out, "&quot;", 6);
            return 6;
          }
          break;
        case '\'':
          if (type == GLTANG_UNICODE_STRING_TYPE_HTML_ATTRIBUTE) {
            memcpy(out, "&#39;", 5);
            return 5;
          }
          break;
        default:
          break;
      }
      out[0] = (char)c;
      return 1;
    case GLTANG_UNICODE_STRING_TYPE_PERCENT:
      if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-' || c == '_' || c == '.' || c == '~') {
        out[0] = (char)c;
        return 1;
      }
      if (c == ' ') {
        out[0] = '+';
        return 1;
      }
      out[0] = '%';
      out[1] = "0123456789ABCDEF"[c >> 4];
      out[2] = "0123456789ABCDEF"[c & 0x0F];
      return 3;
    case GLTANG_UNICODE_STRING_TYPE_JAVASCRIPT:
      switch (c) {
        case '\'': case '"': case '\\':
          out[0] = '\\';
          out[1] = (char)c;
          return 2;
        case '\n': memcpy(out, "\\n", 2); return 2;
        case '\r': memcpy(out, "\\r", 2); return 2;
        case '\t': memcpy(out, "\\t", 2); return 2;
        case '<': memcpy(out, "\\u003C", 6); return 6;
        case '>': memcpy(out, "\\u003E", 6); return 6;
        case '&': memcpy(out, "\\u0026", 6); return 6;
        default:
          out[0] = (char)c;
          return 1;
      }
  }
  out[0] = (char)c;
  return 1;
}

/** The segment's byte range [from, to) of segment `i`. */
static void segment_bytes(const GLTANG_StringBlock * s, size_t i, size_t * from, size_t * to) {
  const uint64_t * segments = gltang_string_segments(s);
  size_t first = GLTANG_SEGMENT_FIRST(segments[i]);
  size_t next = i + 1u < s->segment_count ? GLTANG_SEGMENT_FIRST(segments[i + 1u]) : (size_t)s->grapheme_length;
  *from = gltang_string_byte_offset(s, first);
  *to = gltang_string_byte_offset(s, next);
}

bool gltang_vm_render_block(const GLTANG_StringBlock * s, char ** out, size_t * out_length) {
  size_t length = 0;
  for (size_t i = 0; i < s->segment_count; ++i) {
    size_t from, to;
    segment_bytes(s, i, &from, &to);
    GLTANG_String_Type type = GLTANG_SEGMENT_TYPE(gltang_string_segments(s)[i]);
    char scratch[8];
    for (size_t b = from; b < to; ++b) {
      length += encode_byte(type, (unsigned char)gltang_string_bytes(s)[b], scratch);
    }
  }
  char * buffer = gcu_malloc(length + 1u);
  if (!buffer) {
    return false;
  }
  size_t at = 0;
  for (size_t i = 0; i < s->segment_count; ++i) {
    size_t from, to;
    segment_bytes(s, i, &from, &to);
    GLTANG_String_Type type = GLTANG_SEGMENT_TYPE(gltang_string_segments(s)[i]);
    for (size_t b = from; b < to; ++b) {
      at += encode_byte(type, (unsigned char)gltang_string_bytes(s)[b], buffer + at);
    }
  }
  buffer[length] = '\0';
  *out = buffer;
  *out_length = length;
  return true;
}

GLTANG_Value gltang_vm_string_render(GLTANG_Execution * exec, GLTANG_Value v) {
  const GLTANG_StringBlock * x = gltang_vm_string(v);
  Pacer pacer = {exec, 0};
  // Pass one: the size of the encoding.
  size_t length = 0;
  bool has_cr = false;
  for (size_t i = 0; i < x->segment_count; ++i) {
    size_t from, to;
    segment_bytes(x, i, &from, &to);
    GLTANG_String_Type type = GLTANG_SEGMENT_TYPE(gltang_string_segments(x)[i]);
    const char * bytes = gltang_string_bytes(x);
    char scratch[8];
    for (size_t b = from; b < to;) {
      size_t stop = to - b > GLTANG_POLL_BYTES ? b + GLTANG_POLL_BYTES : to;
      size_t chunk = stop - b;
      for (; b < stop; ++b) {
        size_t n = encode_byte(type, (unsigned char)bytes[b], scratch);
        length += n;
        has_cr = has_cr || scratch[0] == '\r';
      }
      if (pace(&pacer, chunk) != GLTANG_ST_OK) {
        return GLTANG_V_UNWIND;
      }
    }
  }
  if (length > UINT32_MAX) {
    return exec->roots[GLTANG_ROOT_OOM];
  }
  bool direct = !gltang_string_has_offsets(x) && !has_cr;
  char * temp = NULL;
  GLTANG_StringBlock * s = NULL;
  char * dest;
  GLTANG_Value result = GLTANG_V_NULL;
  if (direct) {
    GLTANG_Status st = gltang_vm_string_alloc(exec, 1, length, length, &s);
    if (st != GLTANG_ST_OK) {
      return gltang_vm_failure_value(exec, st);
    }
    gltang_string_segments(s)[0] = GLTANG_SEGMENT_WORD(GLTANG_UNICODE_STRING_TYPE_TRUSTED, 0);
    result = gltang_value_of(s);
    if (!gltang_vm_temp_push(exec, result)) {
      return exec->roots[GLTANG_ROOT_OOM];
    }
    dest = gltang_string_bytes(s);
  }
  else {
    temp = gcu_allocator_malloc(exec->allocator, length + 1u);
    if (!temp) {
      return exec->roots[GLTANG_ROOT_OOM];
    }
    dest = temp;
  }
  // Pass two: write it.
  size_t at = 0;
  GLTANG_Status poll = GLTANG_ST_OK;
  const char * bytes = gltang_string_bytes(x);
  for (size_t i = 0; i < x->segment_count && poll == GLTANG_ST_OK; ++i) {
    size_t from, to;
    segment_bytes(x, i, &from, &to);
    GLTANG_String_Type type = GLTANG_SEGMENT_TYPE(gltang_string_segments(x)[i]);
    for (size_t b = from; b < to && poll == GLTANG_ST_OK;) {
      size_t stop = to - b > GLTANG_POLL_BYTES ? b + GLTANG_POLL_BYTES : to;
      size_t chunk = stop - b;
      for (; b < stop; ++b) {
        at += encode_byte(type, (unsigned char)bytes[b], dest + at);
      }
      poll = pace(&pacer, chunk);
    }
  }
  if (direct) {
    gltang_vm_temp_pop(exec);
    return poll != GLTANG_ST_OK ? GLTANG_V_UNWIND : result;
  }
  if (poll != GLTANG_ST_OK) {
    gcu_allocator_free(exec->allocator, temp);
    return GLTANG_V_UNWIND;
  }
  temp[length] = '\0';
  // The encoded text is made into a string the ordinary way, which breaks it
  // into graphemes again: a rendered string is an ordinary TRUSTED string.
  GLTANG_Value rendered = gltang_vm_string_from_utf8(exec, temp, length, GLTANG_UNICODE_STRING_TYPE_TRUSTED);
  gcu_allocator_free(exec->allocator, temp);
  if (rendered == GLTANG_V_NULL) {
    return exec->roots[GLTANG_ROOT_OOM];
  }
  return rendered;
}


bool gltang_vm_render_segments(const char * bytes, size_t length, const GLTANG_OutputSegment * segments, size_t segment_count, char ** out, size_t * out_length) {
  size_t total = 0;
  char scratch[8];
  for (size_t i = 0; i < segment_count; ++i) {
    size_t to = i + 1u < segment_count ? segments[i + 1u].offset : length;
    for (size_t b = segments[i].offset; b < to; ++b) {
      total += encode_byte(segments[i].type, (unsigned char)bytes[b], scratch);
    }
  }
  char * buffer = gcu_malloc(total + 1u);
  if (!buffer) {
    return false;
  }
  size_t at = 0;
  for (size_t i = 0; i < segment_count; ++i) {
    size_t to = i + 1u < segment_count ? segments[i + 1u].offset : length;
    for (size_t b = segments[i].offset; b < to; ++b) {
      at += encode_byte(segments[i].type, (unsigned char)bytes[b], buffer + at);
    }
  }
  buffer[total] = '\0';
  *out = buffer;
  *out_length = total;
  return true;
}
