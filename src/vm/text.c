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
 * Text: the sinks that output and built strings are written to, number
 * formatting, and the rendering of a value (what `print` appends and what a
 * container shows for an element).
 *
 * A sink takes pieces with their encodings. Output is a sequence of typed
 * segments (language reference, section 8.2), a built string is a string with
 * the same segments and its grapheme boundaries, and a buffer sink fills a
 * fixed buffer as snprintf does, for the host and for the debugger's
 * inspector. Appending to the first two is paced by the runtime poll.
 */

#include <ghoti.io/lang-tang/macros.h>

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ghoti.io/cutil/memory.h>
#include "vm_internal.h"

// ---------------------------------------------------------------------------
// Number formatting
// ---------------------------------------------------------------------------

size_t gltang_vm_format_integer(int64_t n, char * buffer) {
  char digits[24];
  size_t count = 0;
  uint64_t magnitude = n < 0 ? (uint64_t)0 - (uint64_t)n : (uint64_t)n;
  do {
    digits[count++] = (char)('0' + (int)(magnitude % 10u));
    magnitude /= 10u;
  } while (magnitude);
  size_t at = 0;
  if (n < 0) {
    buffer[at++] = '-';
  }
  while (count) {
    buffer[at++] = digits[--count];
  }
  buffer[at] = '\0';
  return at;
}

size_t gltang_vm_format_float(double d, char * buffer, size_t size) {
  // Fixed notation, six decimals, trailing zeros removed and the point kept:
  // 3.5, 0.333333, 100., 1. (language reference, section 4.12).
  //
  // Every NaN is "nan". printf says "-nan" for a NaN with its sign bit set and
  // may carry a payload, and which NaN an operation produces depends on the CPU
  // (inf - inf is negative on x86-64 and positive on arm64) and, when both
  // operands are NaN, on operand order. The sign and payload of a NaN are not
  // observable in Tang (documentation/language-reference.md, divergence D-031).
  // (d != d, not isnan(d): MinGW's isnan converts to float where -Wfloat-conversion
  // sees it, as in float_to_int.)
  if (d != d) {
    if (size == 0) {
      return 0;
    }
    size_t n = size > 3u ? 3u : size - 1u;
    memcpy(buffer, "nan", n);
    buffer[n] = '\0';
    return n;
  }
  int written = snprintf(buffer, size, "%f", d);
  if (written < 0) {
    buffer[0] = '\0';
    return 0;
  }
  size_t length = (size_t)written < size ? (size_t)written : size - 1u;
  // printf reads the locale's decimal point. The language's is always '.'.
  const struct lconv * lc = localeconv();
  const char * point = lc && lc->decimal_point ? lc->decimal_point : ".";
  size_t point_length = strlen(point);
  if (point_length && !(point_length == 1 && point[0] == '.')) {
    char * at = strstr(buffer, point);
    if (at) {
      *at = '.';
      memmove(at + 1, at + point_length, strlen(at + point_length) + 1u);
      length = strlen(buffer);
    }
  }
  if (memchr(buffer, '.', length)) {
    while (length > 0 && buffer[length - 1u] == '0') {
      --length;
    }
    buffer[length] = '\0';
  }
  return length;
}

// ---------------------------------------------------------------------------
// Sinks
// ---------------------------------------------------------------------------

void gltang_sink_init(GLTANG_Sink * sink, GLTANG_SinkMode mode, GLTANG_Execution * exec) {
  memset(sink, 0, sizeof(*sink));
  sink->mode = mode;
  sink->exec = exec;
}

void gltang_sink_init_buffer(GLTANG_Sink * sink, char * buffer, size_t size) {
  memset(sink, 0, sizeof(*sink));
  sink->mode = GLTANG_SINK_BUFFER;
  sink->buffer = buffer;
  sink->buffer_size = size;
  if (size) {
    buffer[0] = '\0';
  }
}

void gltang_sink_free(GLTANG_Sink * sink) {
  if (sink->exec) {
    gcu_allocator_free(sink->exec->allocator, sink->bytes);
    gcu_allocator_free(sink->exec->allocator, sink->boundaries);
    gcu_allocator_free(sink->exec->allocator, sink->segments);
  }
  sink->bytes = NULL;
  sink->boundaries = NULL;
  sink->segments = NULL;
}

/** Grows `*pointer` to hold `needed` elements, from `first` elements when it has none, doubling. */
static bool grow_from(GLTANG_Execution * exec, void ** pointer, size_t * capacity, size_t needed, size_t element, size_t first) {
  if (needed <= *capacity) {
    return true;
  }
  size_t grown = *capacity ? *capacity : first;
  while (grown < needed) {
    if (grown > SIZE_MAX / 2u) {
      return false;
    }
    grown *= 2u;
  }
  if (grown > SIZE_MAX / element) {
    return false;
  }
  void * resized = gcu_allocator_realloc(exec->allocator, *pointer, grown * element);
  if (!resized) {
    return false;
  }
  *pointer = resized;
  *capacity = grown;
  return true;
}

static bool grow(GLTANG_Execution * exec, void ** pointer, size_t * capacity, size_t needed, size_t element) {
  return grow_from(exec, pointer, capacity, needed, element, 64u);
}

/**
 * The segments an output starts with. Every template call has an output of its own, which its
 * first print makes, and most calls end with a few segments (one per change of tag). Sixty-four
 * (1 KiB) was more than the allocator's small-block cache serves, so every call's first print
 * went to glibc's general allocator, whose cost depended on how the call's other blocks had been
 * freed (story 4e: about 260 instructions a call compiled, against about 100 interpreted, for the
 * same allocations); four (64 bytes) is served from that cache, and the array doubles as before.
 */
#define GLTANG_OUTPUT_FIRST_SEGMENTS 4u

/** Paces the work of an append: a bytes-per-fuel charge and a poll per chunk. */
static GLTANG_Status sink_pace(GLTANG_Sink * sink, size_t bytes) {
  // A poll happens whenever a chunk's worth has been appended.
  sink->pace_since += bytes;
  if (sink->pace_since >= GLTANG_POLL_BYTES) {
    size_t work = sink->pace_since / GLTANG_WORK_BYTES_PER_FUEL;
    sink->pace_since = 0;
    return sink->mode == GLTANG_SINK_OUTPUT ? GLTANG_NATIVE_POLL(sink->exec, PRINT, work ? work : 1)
                                            : GLTANG_NATIVE_POLL(sink->exec, RENDER_TO_STRING, work ? work : 1);
  }
  return GLTANG_ST_OK;
}

static bool output_add_segment(GLTANG_Execution * exec, GLTANG_OutBuf * out, GLTANG_String_Type type) {
  if (out->segment_count && out->segments[out->segment_count - 1u].type == type) {
    return true;
  }
  void * segments = out->segments;
  if (!grow_from(exec, &segments, &out->segment_capacity, out->segment_count + 1u, sizeof(GLTANG_OutputSegment), GLTANG_OUTPUT_FIRST_SEGMENTS)) {
    return false;
  }
  out->segments = segments;
  out->segments[out->segment_count++] = (GLTANG_OutputSegment){.offset = out->length, .type = type};
  return true;
}

/** Makes room in the execution's output for `length` more bytes carrying `type`. */
static GLTANG_Status output_prepare(GLTANG_Sink * sink, size_t length, GLTANG_String_Type type) {
  GLTANG_Execution * exec = sink->exec;
  GLTANG_OutBuf * out = exec->out;
  void * output = out->bytes;
  if (!grow(exec, &output, &out->capacity, out->length + length + 1u, 1)) {
    return GLTANG_ST_OOM;
  }
  out->bytes = output;
  if (!output_add_segment(exec, out, type)) {
    return GLTANG_ST_OOM;
  }
  return GLTANG_ST_OK;
}

/** Appends bytes to the execution's output, with the tag they carry. `text` is
 *  not in the heap: it is read across the polls this makes. */
static GLTANG_Status output_append(GLTANG_Sink * sink, const char * text, size_t length, GLTANG_String_Type type) {
  GLTANG_Execution * exec = sink->exec;
  GLTANG_OutBuf * out = exec->out;
  if (!length) {
    return GLTANG_ST_OK;
  }
  GLTANG_Status prepared = output_prepare(sink, length, type);
  if (prepared != GLTANG_ST_OK) {
    return prepared;
  }
  for (size_t done = 0; done < length;) {
    size_t n = length - done;
    if (n > GLTANG_POLL_BYTES) {
      n = GLTANG_POLL_BYTES;
    }
    memcpy(out->bytes + out->length, text + done, n);
    out->length += n;
    out->bytes[out->length] = '\0';
    done += n;
    GLTANG_Status st = sink_pace(sink, n);
    if (st != GLTANG_ST_OK) {
      return st;
    }
  }
  return GLTANG_ST_OK;
}

static GLTANG_Status string_add_segment(GLTANG_Sink * sink, GLTANG_String_Type type) {
  GLTANG_Execution * exec = sink->exec;
  size_t graphemes = sink->boundary_count ? sink->boundary_count - 1u : 0;
  if (sink->segment_count && GLTANG_SEGMENT_TYPE(sink->segments[sink->segment_count - 1u]) == type) {
    return GLTANG_ST_OK;
  }
  void * segments = sink->segments;
  if (!grow(exec, &segments, &sink->segment_capacity, sink->segment_count + 1u, sizeof(uint64_t))) {
    return GLTANG_ST_OOM;
  }
  sink->segments = segments;
  sink->segments[sink->segment_count++] = GLTANG_SEGMENT_WORD(type, graphemes);
  return GLTANG_ST_OK;
}

static GLTANG_Status string_reserve(GLTANG_Sink * sink, size_t bytes, size_t graphemes) {
  GLTANG_Execution * exec = sink->exec;
  if (sink->length + bytes > UINT32_MAX) {
    return GLTANG_ST_OOM;
  }
  void * data = sink->bytes;
  if (!grow(exec, &data, &sink->capacity, sink->length + bytes + 1u, 1)) {
    return GLTANG_ST_OOM;
  }
  sink->bytes = data;
  void * boundaries = sink->boundaries;
  if (!grow(exec, &boundaries, &sink->boundary_capacity, sink->boundary_count + graphemes + 1u, sizeof(uint32_t))) {
    return GLTANG_ST_OOM;
  }
  sink->boundaries = boundaries;
  if (!sink->boundary_count) {
    sink->boundaries[0] = 0;
    sink->boundary_count = 1;
  }
  return GLTANG_ST_OK;
}

GLTANG_Status gltang_sink_text(GLTANG_Sink * sink, const char * text, size_t length, GLTANG_String_Type type) {
  if (!length) {
    return GLTANG_ST_OK;
  }
  switch (sink->mode) {
    case GLTANG_SINK_OUTPUT:
      return output_append(sink, text, length, type);
    case GLTANG_SINK_BUFFER:
      if (sink->buffer_size && sink->needed < sink->buffer_size - 1u) {
        size_t room = sink->buffer_size - 1u - sink->needed;
        size_t n = length < room ? length : room;
        memcpy(sink->buffer + sink->needed, text, n);
        sink->buffer[sink->needed + n] = '\0';
      }
      sink->needed += length;
      return GLTANG_ST_OK;
    case GLTANG_SINK_STRING: {
      // Plain text of this kind is ASCII: every byte is a grapheme.
      GLTANG_Status st = string_reserve(sink, length, length);
      if (st == GLTANG_ST_OK) {
        st = string_add_segment(sink, type);
      }
      if (st != GLTANG_ST_OK) {
        return st;
      }
      memcpy(sink->bytes + sink->length, text, length);
      for (size_t i = 1; i <= length; ++i) {
        sink->boundaries[sink->boundary_count++] = (uint32_t)(sink->length + i);
      }
      sink->length += length;
      return sink_pace(sink, length);
    }
  }
  return GLTANG_ST_OK;
}

static GLTANG_Status sink_block(GLTANG_Sink * sink, const GLTANG_StringBlock * s) {
  if (!s->byte_length) {
    return GLTANG_ST_OK;
  }
  if (sink->mode != GLTANG_SINK_STRING) {
    // Output and buffer take the segments one at a time.
    const uint64_t * segments = gltang_string_segments(s);
    for (size_t i = 0; i < s->segment_count; ++i) {
      size_t first = GLTANG_SEGMENT_FIRST(segments[i]);
      size_t next = i + 1u < s->segment_count ? GLTANG_SEGMENT_FIRST(segments[i + 1u]) : (size_t)s->grapheme_length;
      size_t from = gltang_string_byte_offset(s, first);
      size_t to = gltang_string_byte_offset(s, next);
      GLTANG_Status st = gltang_sink_text(sink, gltang_string_bytes(s) + from, to - from, GLTANG_SEGMENT_TYPE(segments[i]));
      if (st != GLTANG_ST_OK) {
        return st;
      }
    }
    return GLTANG_ST_OK;
  }
  GLTANG_Status st = string_reserve(sink, (size_t)s->byte_length, (size_t)s->grapheme_length);
  if (st != GLTANG_ST_OK) {
    return st;
  }
  const uint64_t * segments = gltang_string_segments(s);
  size_t base = sink->boundary_count - 1u;
  for (size_t i = 0; i < s->segment_count; ++i) {
    // A segment starts at the grapheme it starts at in the result, unless it
    // continues the segment before it.
    if (sink->segment_count && GLTANG_SEGMENT_TYPE(sink->segments[sink->segment_count - 1u]) == GLTANG_SEGMENT_TYPE(segments[i])) {
      continue;
    }
    void * grown = sink->segments;
    if (!grow(sink->exec, &grown, &sink->segment_capacity, sink->segment_count + 1u, sizeof(uint64_t))) {
      return GLTANG_ST_OOM;
    }
    sink->segments = grown;
    sink->segments[sink->segment_count++] = GLTANG_SEGMENT_WORD(GLTANG_SEGMENT_TYPE(segments[i]), base + GLTANG_SEGMENT_FIRST(segments[i]));
  }
  const uint32_t * offsets = gltang_string_offsets(s);
  for (size_t g = 1; g <= s->grapheme_length; ++g) {
    sink->boundaries[sink->boundary_count++] = (uint32_t)(sink->length + (offsets ? offsets[g] : g));
  }
  memcpy(sink->bytes + sink->length, gltang_string_bytes(s), (size_t)s->byte_length);
  sink->length += (size_t)s->byte_length;
  return sink_pace(sink, (size_t)s->byte_length);
}

/**
 * A string of the heap into the execution's output. Each chunk is copied and
 * then the sink polls, and a poll can move the string, so it is held and its
 * bytes are found again after every one (output_append reads host text, which
 * does not move).
 */
static GLTANG_Status output_string(GLTANG_Sink * sink, GLTANG_Value value) {
  GLTANG_Execution * exec = sink->exec;
  if (!gltang_vm_string(value)->byte_length) {
    return GLTANG_ST_OK;
  }
  size_t mark = gltang_vm_temp_mark(exec);
  if (!gltang_vm_temp_push(exec, value)) {
    return GLTANG_ST_OOM;
  }
  GLTANG_Status st = GLTANG_ST_OK;
  size_t segment_count = gltang_vm_string(value)->segment_count;
  for (size_t i = 0; i < segment_count && st == GLTANG_ST_OK; ++i) {
    const GLTANG_StringBlock * s = gltang_vm_string(gltang_vm_temp_at(exec, mark));
    const uint64_t * segments = gltang_string_segments(s);
    size_t first = GLTANG_SEGMENT_FIRST(segments[i]);
    size_t next = i + 1u < s->segment_count ? GLTANG_SEGMENT_FIRST(segments[i + 1u]) : (size_t)s->grapheme_length;
    size_t from = gltang_string_byte_offset(s, first);
    size_t length = gltang_string_byte_offset(s, next) - from;
    GLTANG_String_Type type = GLTANG_SEGMENT_TYPE(segments[i]);
    if (!length) {
      continue;
    }
    st = output_prepare(sink, length, type);
    GLTANG_OutBuf * out = exec->out;
    for (size_t done = 0; st == GLTANG_ST_OK && done < length;) {
      size_t n = length - done;
      if (n > GLTANG_POLL_BYTES) {
        n = GLTANG_POLL_BYTES;
      }
      s = gltang_vm_string(gltang_vm_temp_at(exec, mark));
      memcpy(out->bytes + out->length, gltang_string_bytes(s) + from + done, n);
      out->length += n;
      out->bytes[out->length] = '\0';
      done += n;
      st = sink_pace(sink, n);
    }
  }
  gltang_vm_temp_release(exec, mark);
  return st;
}

GLTANG_Status gltang_sink_string(GLTANG_Sink * sink, GLTANG_Value s) {
  if (sink->mode == GLTANG_SINK_OUTPUT) {
    return output_string(sink, s);
  }
  // The other two copy the whole string before anything can poll.
  return sink_block(sink, gltang_vm_string(s));
}

GLTANG_Value gltang_sink_finish(GLTANG_Sink * sink) {
  size_t graphemes = sink->boundary_count ? sink->boundary_count - 1u : 0;
  size_t segments = sink->segment_count ? sink->segment_count : 1u;
  GLTANG_StringBlock * s;
  GLTANG_Status st = gltang_vm_string_alloc(sink->exec, segments, sink->length, graphemes, &s);
  if (st != GLTANG_ST_OK) {
    return gltang_vm_failure_value(sink->exec, st);
  }
  if (sink->segment_count) {
    memcpy(gltang_string_segments(s), sink->segments, sink->segment_count * sizeof(uint64_t));
  }
  else {
    gltang_string_segments(s)[0] = GLTANG_SEGMENT_WORD(GLTANG_UNICODE_STRING_TYPE_TRUSTED, 0);
  }
  uint32_t * offsets = gltang_string_offsets(s);
  if (offsets) {
    memcpy(offsets, sink->boundaries, (graphemes + 1u) * sizeof(uint32_t));
  }
  if (sink->length) {
    memcpy(gltang_string_bytes(s), sink->bytes, sink->length);
  }
  return gltang_value_of(s);
}

// ---------------------------------------------------------------------------
// Rendering a value
// ---------------------------------------------------------------------------

static GLTANG_Status text(GLTANG_Sink * sink, const char * s) {
  return gltang_sink_text(sink, s, strlen(s), GLTANG_UNICODE_STRING_TYPE_TRUSTED);
}

static bool sink_full(const GLTANG_Sink * sink) {
  return sink->limit && sink->needed >= sink->limit;
}

// The digits of a number are formatted in a frame of their own. The renderer
// recurses once per level of a container, up to GLTANG_MAX_VALUE_DEPTH, and a
// 512-byte buffer in the recursive function's own frame was paid at every
// level whether the level printed a number or not: value depth 2048 needed
// about 1 MiB of C stack, which a host thread with a small stack does not have.
// (noinline, so that the buffer cannot come back into the recursive frame.)
/**
 * A float's text, given the double: the one place a float's text is made for a sink. A float
 * object's (render_number) is this, and so is what compiled code's raw float becomes in a print, a
 * concatenation and a cast to string (spec-template-compiled story 4g), so a float has one text
 * whichever tier holds it, boxed or raw. Inlined where it is called, so that its buffer is the frame of
 * render_number (kept out of the recursive renderer, below) or of the function compiled code calls.
 */
GLTANG_VM_SHARED GLTANG_Status render_float(GLTANG_Sink * sink, double d) {
  char digits[512];
  size_t n = gltang_vm_format_float(d, digits, sizeof(digits));
  return gltang_sink_text(sink, digits, n, GLTANG_UNICODE_STRING_TYPE_TRUSTED);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static GLTANG_Status render_number(GLTANG_Sink * sink, GLTANG_Value v, GLTANG_ValueKind kind) {
  if (kind != GLTANG_KIND_INTEGER) {
    return render_float(sink, gltang_vm_float(v));
  }
  char digits[24];
  size_t n = gltang_vm_format_integer(gltang_vm_int(v), digits);
  return gltang_sink_text(sink, digits, n, GLTANG_UNICODE_STRING_TYPE_TRUSTED);
}

/**
 * A container held while its elements are rendered. Every piece a sink takes can
 * poll (all but a buffer sink's, which never does), and a poll can move the
 * container, so it is held in a temporary and found again from there before
 * each element is read (the rules at the top of vm_internal.h).
 */
typedef struct Held {
  GLTANG_Execution * exec;
  size_t mark;
  GLTANG_Value plain;
  bool on;
} Held;

static bool hold(const GLTANG_Sink * sink, GLTANG_Value v, Held * h) {
  h->on = sink->exec != NULL && sink->mode != GLTANG_SINK_BUFFER;
  h->plain = v;
  h->exec = sink->exec;
  h->mark = 0;
  if (!h->on) {
    return true;
  }
  h->mark = gltang_vm_temp_mark(h->exec);
  return gltang_vm_temp_push(h->exec, v);
}

static GLTANG_Value held(const Held * h) {
  return h->on ? gltang_vm_temp_at(h->exec, h->mark) : h->plain;
}

static void unhold(const Held * h) {
  if (h->on) {
    gltang_vm_temp_release(h->exec, h->mark);
  }
}

GLTANG_Status gltang_vm_render(GLTANG_Sink * sink, GLTANG_Value v, GLTANG_RenderMode mode, int depth, bool * too_deep) {
  switch (gltang_vm_kind(v)) {
    case GLTANG_KIND_NULL:
      return mode == GLTANG_RENDER_ELEMENT ? text(sink, "null") : GLTANG_ST_OK;
    case GLTANG_KIND_BOOL:
      return text(sink, v == GLTANG_V_TRUE ? "true" : "false");
    case GLTANG_KIND_INTEGER:
      return render_number(sink, v, GLTANG_KIND_INTEGER);
    case GLTANG_KIND_FLOAT:
      return render_number(sink, v, GLTANG_KIND_FLOAT);
    case GLTANG_KIND_STRING:
      return gltang_sink_string(sink, v);
    case GLTANG_KIND_ARRAY: {
      if (depth >= GLTANG_MAX_VALUE_DEPTH) {
        *too_deep = true;
        return GLTANG_ST_OK;
      }
      Held h;
      if (!hold(sink, v, &h)) {
        return GLTANG_ST_OOM;
      }
      GLTANG_Status st = text(sink, "[");
      for (uint64_t i = 0; st == GLTANG_ST_OK && !*too_deep && !sink_full(sink) && i < gltang_vm_array(held(&h))->length; ++i) {
        if (i) {
          st = text(sink, ", ");
        }
        if (st == GLTANG_ST_OK) {
          st = gltang_vm_render(sink, gltang_vm_array(held(&h))->store.typed->slots[i], GLTANG_RENDER_ELEMENT, depth + 1, too_deep);
        }
      }
      unhold(&h);
      return st == GLTANG_ST_OK ? text(sink, "]") : st;
    }
    case GLTANG_KIND_MAP: {
      if (depth >= GLTANG_MAX_VALUE_DEPTH) {
        *too_deep = true;
        return GLTANG_ST_OK;
      }
      Held h;
      if (!hold(sink, v, &h)) {
        return GLTANG_ST_OOM;
      }
      GLTANG_Status st = text(sink, "{");
      for (uint64_t i = 0; st == GLTANG_ST_OK && !*too_deep && !sink_full(sink) && i < gltang_vm_map(held(&h))->count; ++i) {
        if (i) {
          st = text(sink, ", ");
        }
        if (st == GLTANG_ST_OK) {
          st = text(sink, "\"");
        }
        if (st == GLTANG_ST_OK) {
          st = gltang_sink_string(sink, gltang_vm_map(held(&h))->store.typed->slots[2u * i]);
        }
        if (st == GLTANG_ST_OK) {
          st = text(sink, "\": ");
        }
        if (st == GLTANG_ST_OK) {
          st = gltang_vm_render(sink, gltang_vm_map(held(&h))->store.typed->slots[2u * i + 1u], GLTANG_RENDER_ELEMENT, depth + 1, too_deep);
        }
      }
      unhold(&h);
      return st == GLTANG_ST_OK ? text(sink, "}") : st;
    }
    case GLTANG_KIND_FUNCTION: {
      if (mode == GLTANG_RENDER_PRINT) {
        return GLTANG_ST_OK;
      }
      char name[48];
      if (gltang_v_is_function(v)) {
        uint64_t index = gltang_v_function_index(v);
        unsigned parameters = 0;
        const GLTANG_Program * program = sink->exec && sink->exec->act ? gltang_exec_current_program(sink->exec) : NULL;
        if (program && index < program->function_count) {
          parameters = program->functions[index].parameter_count;
        }
        snprintf(name, sizeof(name), "Function(%u)", parameters);
      }
      else {
        // A template takes no arguments; a host function's count is its own.
        snprintf(name, sizeof(name), "%s", gltang_object_kind(v) == GLTANG_OBJ_TEMPLATE ? "Function(0)" : "Function(native)");
      }
      return text(sink, name);
    }
    case GLTANG_KIND_LIBRARY: {
      if (mode == GLTANG_RENDER_PRINT) {
        return GLTANG_ST_OK;
      }
      const char * library_name = gltang_library_name(((const GLTANG_LibraryObject *)gltang_object(v))->library);
      GLTANG_Status st = text(sink, "Library: ");
      return st == GLTANG_ST_OK && library_name ? text(sink, library_name) : st;
    }
    case GLTANG_KIND_RNG:
      return mode == GLTANG_RENDER_PRINT ? GLTANG_ST_OK : text(sink, "RNG");
    case GLTANG_KIND_CUSTOM: {
      // As a function, until a type can print itself (story 6): `print`
      // appends nothing, and an element shows the type's name.
      if (mode == GLTANG_RENDER_PRINT) {
        return GLTANG_ST_OK;
      }
      GLTANG_Status st = text(sink, "Custom: ");
      return st == GLTANG_ST_OK ? text(sink, gltang_type_name(gltang_vm_custom(v)->type)) : st;
    }
    case GLTANG_KIND_ERROR: {
      GLTANG_ErrorKind kind = gltang_vm_error_kind(v);
      if (gltang_error_kind_is_marker(kind)) {
        return text(sink, gltang_error_kind_message(kind));
      }
      if (mode == GLTANG_RENDER_PRINT) {
        return GLTANG_ST_OK;
      }
      GLTANG_Status st = text(sink, "Error: ");
      return st == GLTANG_ST_OK ? text(sink, gltang_error_kind_message(kind)) : st;
    }
  }
  return GLTANG_ST_OK;
}

/** Drops what a print that failed appended, back to the last finished print. */
static void output_rollback(GLTANG_OutBuf * out) {
  if (out->length > out->committed) {
    out->length = out->committed;
    if (out->bytes) {
      out->bytes[out->length] = '\0';
    }
    while (out->segment_count && out->segments[out->segment_count - 1u].offset >= out->length) {
      --out->segment_count;
    }
  }
}

/** Whether a print that finishes is handed to the host's sink: only the main activation's output is. */
static bool output_delivers(const GLTANG_Execution * exec) {
  return exec->sink && exec->out == &exec->main_act.out;
}

/**
 * Gives the output sink what a print has just finished: the segments from the
 * last delivered point to the end of the output, each as the bytes of one run of
 * one tag. Only the main activation's output is delivered. Returns false if the
 * sink refused; `sink_delivered` then still marks the start of this print.
 */
static bool output_deliver(GLTANG_Execution * exec) {
  GLTANG_OutBuf * out = exec->out;
  if (!output_delivers(exec) || out->length <= exec->sink_delivered) {
    return true;
  }
  const size_t from = exec->sink_delivered;
  const size_t to = out->length;
  // The segment holding `from`: segments are contiguous and in order.
  size_t first = out->segment_count;
  while (first > 0 && out->segments[first - 1u].offset > from) {
    --first;
  }
  first = first ? first - 1u : 0;
  const bool outer_in_host = exec->in_host;
  exec->in_host = true;
  bool taken = true;
  for (size_t i = first; taken && i < out->segment_count; ++i) {
    size_t start = out->segments[i].offset > from ? out->segments[i].offset : from;
    size_t end = i + 1u < out->segment_count ? out->segments[i + 1u].offset : to;
    end = end > to ? to : end;
    if (end > start) {
      taken = exec->sink(exec->sink_user, out->bytes + start, end - start, out->segments[i].type);
    }
  }
  exec->in_host = outer_in_host;
  if (taken) {
    exec->sink_delivered = to;
  }
  return taken;
}

/** What a print does once its value's text is appended (`st`, `too_deep`): the rollback and the error of a failure, or the delivery and the commit. */
GLTANG_VM_SHARED GLTANG_Value print_finish(GLTANG_Execution * exec, GLTANG_Status st, bool too_deep) {
  if (st != GLTANG_ST_OK || too_deep) {
    output_rollback(exec->out);
    return st != GLTANG_ST_OK ? gltang_vm_failure_value(exec, st) : gltang_vm_make_error(exec, GLTANG_ERROR_RECURSION_LIMIT);
  }
  if (!output_deliver(exec)) {
    output_rollback(exec->out);
    return gltang_vm_make_error(exec, GLTANG_ERROR_HOST_FAILED);
  }
  exec->out->committed = exec->out->length;
  return GLTANG_V_NULL;
}

GLTANG_Value gltang_vm_op_print(GLTANG_Execution * exec, GLTANG_Value v) {
  // An error prints as nothing (a marker prints itself): it is swallowed here.
  gltang_vm_error_swallowed(exec, GLTANG_ERROR_HOW_PRINTED, v, exec->act);
  GLTANG_Sink sink;
  gltang_sink_init(&sink, GLTANG_SINK_OUTPUT, exec);
  bool too_deep = false;
  GLTANG_Status st = gltang_vm_render(&sink, v, GLTANG_RENDER_PRINT, 0, &too_deep);
  return print_finish(exec, st, too_deep);
}

GLTANG_Value gltang_vm_print_float(GLTANG_Execution * exec, double d) {
  // gltang_vm_op_print of a float object, whose error swallow does nothing (a float is no error)
  // and whose render is this one piece.
  GLTANG_Sink sink;
  gltang_sink_init(&sink, GLTANG_SINK_OUTPUT, exec);
  return print_finish(exec, render_float(&sink, d), false);
}

GLTANG_Value gltang_vm_print_constant(GLTANG_Execution * exec, const GLTANG_StringBlock * s) {
  GLTANG_Sink sink;
  gltang_sink_init(&sink, GLTANG_SINK_OUTPUT, exec);
  GLTANG_Status st = sink_block(&sink, s);
  if (st == GLTANG_ST_OK) {
    if (!output_deliver(exec)) {
      output_rollback(exec->out);
      return gltang_vm_make_error(exec, GLTANG_ERROR_HOST_FAILED);
    }
    exec->out->committed = exec->out->length;
    return GLTANG_V_NULL;
  }
  output_rollback(exec->out);
  return gltang_vm_failure_value(exec, st);
}

// ---------------------------------------------------------------------------
// The fast paths of compiled code (spec-template-compiled story 4d)
// ---------------------------------------------------------------------------
//
// A second implementation, of the common case only, of the two functions above, which compiled
// code calls without the native-call bracket and which must equal them in every observable. Each
// answers false, having changed nothing, unless the print is one in which the function above
// would only copy the bytes: then it copies them as that function would and answers true. What
// the function above does beyond the copy, and why none of it can happen here:
//
// - a poll: `sink_pace` polls only once a print's bytes reach GLTANG_POLL_BYTES (`pace_since`
//   starts at 0 for each print), so a text of fewer bytes never polls;
// - an allocation: `output_prepare` grows nothing when the bytes and their terminator fit the
//   capacity, and `output_add_segment` adds nothing when the last segment has the text's tag;
//   `output_string`'s temporary is pushed into storage that has room;
// - the sink: `output_deliver` does nothing unless output_delivers();
// - a failure, a rollback, an error swallowed (the operand is a string): none.
//
// The fuel and the counters are the caller's (src/jit/helpers.c), charged only when the answer is
// true. tests/unit/test_jit_templates.cpp holds each condition to the slow path it must take.

/** The copy of a print of one run of `length` bytes tagged `type`, if nothing else would happen. */
static bool print_run_fast(GLTANG_Execution * exec, const char * bytes, size_t length, GLTANG_String_Type type) {
  GLTANG_OutBuf * out = exec->out;
  if (length >= GLTANG_POLL_BYTES || output_delivers(exec) || !out->segment_count
      || out->segments[out->segment_count - 1u].type != type || out->length + length + 1u > out->capacity) {
    return false;
  }
  memcpy(out->bytes + out->length, bytes, length);
  out->length += length;
  out->bytes[out->length] = '\0';
  out->committed = out->length;
  return true;
}

/** The bytes and the tag of a string's one segment, as `sink_block` and `output_string` find them. */
static bool one_run(const GLTANG_StringBlock * s, const char ** bytes, size_t * length, GLTANG_String_Type * type) {
  if (s->segment_count != 1u) {
    return false;
  }
  const uint64_t segment = gltang_string_segments(s)[0];
  const size_t from = gltang_string_byte_offset(s, GLTANG_SEGMENT_FIRST(segment));
  *bytes = gltang_string_bytes(s) + from;
  *length = gltang_string_byte_offset(s, (size_t)s->grapheme_length) - from;
  *type = GLTANG_SEGMENT_TYPE(segment);
  return true;
}

bool gltang_vm_print_constant_fast(GLTANG_Execution * exec, const GLTANG_StringBlock * s) {
  const char * bytes;
  size_t length;
  GLTANG_String_Type type;
  return one_run(s, &bytes, &length, &type) && print_run_fast(exec, bytes, length, type);
}

bool gltang_vm_print_string_fast(GLTANG_Execution * exec, GLTANG_Value v) {
  if (!gltang_v_is_kind(v, GLTANG_OBJ_STRING) || exec->temp_count >= exec->temp_capacity) {
    return false;
  }
  const char * bytes;
  size_t length;
  GLTANG_String_Type type;
  return one_run(gltang_vm_string(v), &bytes, &length, &type) && print_run_fast(exec, bytes, length, type);
}

bool gltang_vm_print_float_fast(GLTANG_Execution * exec, double d) {
  // The text is made as render_float makes it; a float's is at most a few hundred bytes, below the
  // pacing interval, so only the output's room, its last tag and the sink decide (print_run_fast).
  char digits[512];
  size_t n = gltang_vm_format_float(d, digits, sizeof(digits));
  return print_run_fast(exec, digits, n, GLTANG_UNICODE_STRING_TYPE_TRUSTED);
}

GLTANG_Value gltang_vm_output_string(GLTANG_Execution * exec, const GLTANG_OutBuf * out, size_t length) {
  GLTANG_Sink sink;
  gltang_sink_init(&sink, GLTANG_SINK_STRING, exec);
  GLTANG_Status st = GLTANG_ST_OK;
  for (size_t i = 0; i < out->segment_count && st == GLTANG_ST_OK; ++i) {
    size_t from = out->segments[i].offset;
    size_t to = i + 1u < out->segment_count ? out->segments[i + 1u].offset : out->length;
    if (to > length) {
      to = length;
    }
    if (from >= to) {
      continue;
    }
    // Each piece is a string of its own (which re-breaks its graphemes), copied
    // into the result before anything else can collect it.
    GLTANG_Value piece = gltang_vm_string_from_utf8(exec, out->bytes + from, to - from, out->segments[i].type);
    if (piece == GLTANG_V_UNWIND) {
      st = GLTANG_ST_UNWIND;
    }
    else if (gltang_v_is_kind(piece, GLTANG_OBJ_STRING)) {
      st = sink_block(&sink, gltang_vm_string(piece));
    }
    else if (piece != GLTANG_V_NULL) {
      st = GLTANG_ST_OOM;
    }
  }
  GLTANG_Value result = st == GLTANG_ST_OK ? gltang_sink_finish(&sink) : gltang_vm_failure_value(exec, st);
  gltang_sink_free(&sink);
  return result;
}

/** The string a to-string sink built (`st`, `too_deep` as its render left them), or the failure; the sink is freed. */
GLTANG_VM_SHARED GLTANG_Value to_string_finish(GLTANG_Execution * exec, GLTANG_Sink * sink, GLTANG_Status st, bool too_deep) {
  GLTANG_Value result;
  if (st != GLTANG_ST_OK) {
    result = gltang_vm_failure_value(exec, st);
  }
  else if (too_deep) {
    result = gltang_vm_make_error(exec, GLTANG_ERROR_RECURSION_LIMIT);
  }
  else {
    result = gltang_sink_finish(sink);
  }
  gltang_sink_free(sink);
  return result;
}

GLTANG_Value gltang_vm_to_string(GLTANG_Execution * exec, GLTANG_Value v) {
  if (gltang_v_is_kind(v, GLTANG_OBJ_STRING)) {
    return v;
  }
  if (gltang_v_is_custom(v)) {
    // A type has no to-string yet (story 6): never an empty string.
    return gltang_vm_make_error(exec, GLTANG_ERROR_NOT_SUPPORTED);
  }
  GLTANG_Sink sink;
  gltang_sink_init(&sink, GLTANG_SINK_STRING, exec);
  bool too_deep = false;
  GLTANG_Status st = gltang_vm_render(&sink, v, GLTANG_RENDER_ELEMENT, 0, &too_deep);
  return to_string_finish(exec, &sink, st, too_deep);
}

GLTANG_Value gltang_vm_float_to_string(GLTANG_Execution * exec, double d) {
  // gltang_vm_to_string of a float object: not a string, not a host-defined value, and its render
  // is this one piece.
  GLTANG_Sink sink;
  gltang_sink_init(&sink, GLTANG_SINK_STRING, exec);
  return to_string_finish(exec, &sink, render_float(&sink, d), false);
}
