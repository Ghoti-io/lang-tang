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
 * The operations: arithmetic, comparison, casts, indexing, attributes, slices
 * and assignment through them.
 *
 * Written from language reference sections 4 and 13, and checked against
 * ctang's behaviour where the reference is silent (the operand-kind tables are
 * in design.md). An operation that cannot be done is an error value, never an
 * unwind; the only thing that stops a run is GLTANG_V_UNWIND from a runtime
 * poll.
 */

#include <ghoti.io/lang-tang/macros.h>

#include <ctype.h>
#include <locale.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <ghoti.io/cutil/memory.h>
#include "vm_internal.h"

#define ERROR(kind) gltang_vm_make_error(exec, (kind))

// ---------------------------------------------------------------------------
// Pacing, shared by the loops that copy elements
// ---------------------------------------------------------------------------

typedef struct Pacer {
  GLTANG_Execution * exec;
  size_t since;
  GLTANG_NativeId native;  ///< Which native is paced, for the gate.
} Pacer;

static GLTANG_Status pace(Pacer * pacer, size_t elements) {
  pacer->since += elements * 8u;
  if (pacer->since >= GLTANG_POLL_BYTES) {
    size_t work = pacer->since / GLTANG_WORK_BYTES_PER_FUEL;
    pacer->since = 0;
    return gltang_vm_native_poll_as(pacer->exec, pacer->native, work ? work : 1);
  }
  return GLTANG_ST_OK;
}

// ---------------------------------------------------------------------------
// Numbers
// ---------------------------------------------------------------------------

static bool is_integer_value(GLTANG_Value v) {
  return gltang_v_is_small_int(v) || gltang_v_is_kind(v, GLTANG_OBJ_INTEGER);
}

static bool is_float_value(GLTANG_Value v) {
  return gltang_v_is_kind(v, GLTANG_OBJ_FLOAT);
}

static GLTANG_Value overflow(GLTANG_Execution * exec, bool positive) {
  return ERROR(positive ? GLTANG_ERROR_INTEGER_TOO_LARGE : GLTANG_ERROR_INTEGER_TOO_SMALL);
}

static GLTANG_Value arithmetic(GLTANG_Execution * exec, GLTANG_Opcode op, GLTANG_Value a, GLTANG_Value b) {
  if (is_integer_value(a) && is_integer_value(b)) {
    int64_t x = gltang_vm_int(a);
    int64_t y = gltang_vm_int(b);
    int64_t r;
    switch (op) {
      case GLTANG_OP_ADD:
        if (__builtin_add_overflow(x, y, &r)) {
          return overflow(exec, x > 0);
        }
        return gltang_vm_make_int(exec, r);
      case GLTANG_OP_SUB:
        if (__builtin_sub_overflow(x, y, &r)) {
          return overflow(exec, y < 0);
        }
        return gltang_vm_make_int(exec, r);
      case GLTANG_OP_MUL:
        if (__builtin_mul_overflow(x, y, &r)) {
          return overflow(exec, (x < 0) == (y < 0));
        }
        return gltang_vm_make_int(exec, r);
      case GLTANG_OP_DIV:
        if (y == 0) {
          return ERROR(GLTANG_ERROR_DIVIDE_BY_ZERO);
        }
        if (x == INT64_MIN && y == -1) {
          return overflow(exec, true);
        }
        return gltang_vm_make_int(exec, x / y);
      case GLTANG_OP_MOD:
        if (y == 0) {
          return ERROR(GLTANG_ERROR_MODULO_BY_ZERO);
        }
        // x % -1 is 0 for every x, and the hardware trap on INT64_MIN % -1 is
        // never reached.
        return gltang_vm_make_int(exec, y == -1 ? 0 : x % y);
      default:
        break;
    }
    return ERROR(GLTANG_ERROR_NOT_SUPPORTED);
  }
  double x = gltang_vm_number(a);
  double y = gltang_vm_number(b);
  switch (op) {
    case GLTANG_OP_ADD: return gltang_vm_make_float(exec, x + y);
    case GLTANG_OP_SUB: return gltang_vm_make_float(exec, x - y);
    case GLTANG_OP_MUL: return gltang_vm_make_float(exec, x * y);
    case GLTANG_OP_DIV:
      if (y == 0.0) {
        return ERROR(GLTANG_ERROR_DIVIDE_BY_ZERO);
      }
      return gltang_vm_make_float(exec, x / y);
    default:
      // `%` on floats is not supported (4.2).
      return ERROR(GLTANG_ERROR_NOT_SUPPORTED);
  }
}

static GLTANG_Value compare(GLTANG_Execution * exec, GLTANG_Opcode op, GLTANG_Value a, GLTANG_Value b) {
  int order;  // -1, 0, 1, or 2 for unordered (NaN)
  if (gltang_v_is_kind(a, GLTANG_OBJ_STRING) && gltang_v_is_kind(b, GLTANG_OBJ_STRING)) {
    order = gltang_vm_string_compare(a, b);
  }
  else if (is_integer_value(a) && is_integer_value(b)) {
    int64_t x = gltang_vm_int(a);
    int64_t y = gltang_vm_int(b);
    order = x < y ? -1 : (x > y ? 1 : 0);
  }
  else if (gltang_vm_is_number(a) && gltang_vm_is_number(b)) {
    double x = gltang_vm_number(a);
    double y = gltang_vm_number(b);
    order = x < y ? -1 : (x > y ? 1 : (x == y ? 0 : 2));
  }
  else {
    return ERROR(GLTANG_ERROR_NOT_SUPPORTED);
  }
  bool r = false;
  if (order != 2) {
    switch (op) {
      case GLTANG_OP_LT: r = order < 0; break;
      case GLTANG_OP_LE: r = order <= 0; break;
      case GLTANG_OP_GT: r = order > 0; break;
      case GLTANG_OP_GE: r = order >= 0; break;
      default: break;
    }
  }
  return gltang_v_from_bool(r);
}

// ---------------------------------------------------------------------------
// Arrays built from arrays
// ---------------------------------------------------------------------------

/** Appends copies of every element of `source` to `dest`. */
static GLTANG_Status append_copies(GLTANG_Execution * exec, GLTANG_Value dest, GLTANG_Value source, Pacer * pacer, GLTANG_Value * failure) {
  size_t n = (size_t)gltang_vm_array(source)->length;
  for (size_t i = 0; i < n; ++i) {
    GLTANG_Value element = gltang_vm_array(source)->store.typed->slots[i];
    if (gltang_vm_is_container(element)) {
      element = gltang_vm_deep_copy(exec, element);
      if (element == GLTANG_V_UNWIND) {
        *failure = GLTANG_V_UNWIND;
        return GLTANG_ST_UNWIND;
      }
      if (gltang_vm_is_error(element)) {
        // The copy failed: hand back its error, not an array holding one.
        *failure = element;
        return GLTANG_ST_OOM;
      }
      // The copy is held only by this variable until it is pushed; no
      // allocation happens in between.
    }
    gltang_vm_array_push(exec, dest, element);
    GLTANG_Status st = pace(pacer, 1);
    if (st != GLTANG_ST_OK) {
      *failure = GLTANG_V_UNWIND;
      return st;
    }
  }
  return GLTANG_ST_OK;
}

static GLTANG_Value array_concat(GLTANG_Execution * exec, GLTANG_Value a, GLTANG_Value b) {
  size_t total = (size_t)gltang_vm_array(a)->length + (size_t)gltang_vm_array(b)->length;
  GLTANG_Value result = gltang_vm_array_new(exec, total);
  if (!gltang_v_is_kind(result, GLTANG_OBJ_ARRAY)) {
    return result;
  }
  if (!gltang_vm_temp_push(exec, result)) {
    return exec->roots[GLTANG_ROOT_OOM];
  }
  Pacer pacer = GLTANG_PACER(exec, ARRAY_CONCAT);
  GLTANG_Value failure = GLTANG_V_NULL;
  GLTANG_Status st = append_copies(exec, result, a, &pacer, &failure);
  if (st == GLTANG_ST_OK) {
    st = append_copies(exec, result, b, &pacer, &failure);
  }
  gltang_vm_temp_pop(exec);
  return st == GLTANG_ST_OK ? result : failure;
}

static GLTANG_Value array_repeat(GLTANG_Execution * exec, GLTANG_Value a, int64_t count) {
  if (count < 0) {
    return ERROR(GLTANG_ERROR_NOT_SUPPORTED);
  }
  size_t length = (size_t)gltang_vm_array(a)->length;
  if (length == 0) {
    // Repeating nothing is nothing, however many times.
    return gltang_vm_array_new(exec, 0);
  }
  // A count too large to allocate is Out of memory (4.2). The bound is on the
  // element count, not the byte count.
  if ((uint64_t)count > ((uint64_t)1 << 31) / length) {
    return ERROR(GLTANG_ERROR_OUT_OF_MEMORY);
  }
  size_t total = length * (size_t)count;
  GLTANG_Value result = gltang_vm_array_new(exec, total);
  if (!gltang_v_is_kind(result, GLTANG_OBJ_ARRAY)) {
    return result;
  }
  if (!gltang_vm_temp_push(exec, result)) {
    return exec->roots[GLTANG_ROOT_OOM];
  }
  Pacer pacer = GLTANG_PACER(exec, ARRAY_REPEAT);
  GLTANG_Value failure = GLTANG_V_NULL;
  GLTANG_Status st = GLTANG_ST_OK;
  for (int64_t k = 0; k < count && st == GLTANG_ST_OK; ++k) {
    st = append_copies(exec, result, a, &pacer, &failure);
  }
  gltang_vm_temp_pop(exec);
  return st == GLTANG_ST_OK ? result : failure;
}

// ---------------------------------------------------------------------------
// The binary operators
// ---------------------------------------------------------------------------

GLTANG_Value gltang_vm_op_binary(GLTANG_Execution * exec, GLTANG_Opcode op, GLTANG_Value a, GLTANG_Value b) {
  switch (op) {
    case GLTANG_OP_EQ:
    case GLTANG_OP_NE: {
      GLTANG_Value r = gltang_vm_equal(exec, a, b);
      if (r == GLTANG_V_TRUE || r == GLTANG_V_FALSE) {
        return op == GLTANG_OP_EQ ? r : gltang_v_from_bool(r == GLTANG_V_FALSE);
      }
      return r;
    }
    case GLTANG_OP_LT:
    case GLTANG_OP_LE:
    case GLTANG_OP_GT:
    case GLTANG_OP_GE:
      return compare(exec, op, a, b);
    case GLTANG_OP_ADD: {
      bool a_string = gltang_v_is_kind(a, GLTANG_OBJ_STRING);
      bool b_string = gltang_v_is_kind(b, GLTANG_OBJ_STRING);
      if (a_string || b_string) {
        // `+` concatenates when either side is a string, converting the other
        // as printing it would (4.2.1). An error operand is the result.
        GLTANG_Value other = a_string ? b : a;
        if (gltang_vm_is_error(other)) {
          return other;
        }
        GLTANG_ValueKind other_kind = gltang_vm_kind(other);
        if (other_kind == GLTANG_KIND_NULL || other_kind == GLTANG_KIND_FUNCTION || other_kind == GLTANG_KIND_LIBRARY || other_kind == GLTANG_KIND_RNG) {
          return ERROR(GLTANG_ERROR_NOT_SUPPORTED);
        }
        if (a_string && b_string) {
          return gltang_vm_string_concat(exec, a, b);
        }
        GLTANG_Value text = gltang_vm_to_string(exec, other);
        if (!gltang_v_is_kind(text, GLTANG_OBJ_STRING)) {
          return text;
        }
        if (!gltang_vm_temp_push(exec, text)) {
          return exec->roots[GLTANG_ROOT_OOM];
        }
        GLTANG_Value r = a_string ? gltang_vm_string_concat(exec, a, text) : gltang_vm_string_concat(exec, text, b);
        gltang_vm_temp_pop(exec);
        return r;
      }
      if (gltang_vm_is_number(a) && gltang_vm_is_number(b)) {
        return arithmetic(exec, op, a, b);
      }
      if (gltang_v_is_kind(a, GLTANG_OBJ_ARRAY) && gltang_v_is_kind(b, GLTANG_OBJ_ARRAY)) {
        return array_concat(exec, a, b);
      }
      return ERROR(GLTANG_ERROR_NOT_SUPPORTED);
    }
    case GLTANG_OP_MUL:
      if (gltang_v_is_kind(a, GLTANG_OBJ_ARRAY) && is_integer_value(b)) {
        return array_repeat(exec, a, gltang_vm_int(b));
      }
      /* fall through */
    case GLTANG_OP_SUB:
    case GLTANG_OP_DIV:
      if (gltang_vm_is_number(a) && gltang_vm_is_number(b)) {
        return arithmetic(exec, op, a, b);
      }
      return ERROR(GLTANG_ERROR_NOT_SUPPORTED);
    case GLTANG_OP_MOD:
      if (is_integer_value(a) && is_integer_value(b)) {
        return arithmetic(exec, op, a, b);
      }
      return ERROR(GLTANG_ERROR_NOT_SUPPORTED);
    default:
      return ERROR(GLTANG_ERROR_NOT_SUPPORTED);
  }
}

GLTANG_Value gltang_vm_op_neg(GLTANG_Execution * exec, GLTANG_Value v) {
  if (is_integer_value(v)) {
    int64_t n = gltang_vm_int(v);
    if (n == INT64_MIN) {
      return overflow(exec, true);
    }
    return gltang_vm_make_int(exec, -n);
  }
  if (is_float_value(v)) {
    return gltang_vm_make_float(exec, -gltang_vm_float(v));
  }
  return ERROR(GLTANG_ERROR_NOT_SUPPORTED);
}

// ---------------------------------------------------------------------------
// Casts
// ---------------------------------------------------------------------------

static bool is_space(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

static GLTANG_Value string_to_int(GLTANG_Execution * exec, GLTANG_Value v) {
  const GLTANG_StringBlock * s = gltang_vm_string(v);
  const unsigned char * p = (const unsigned char *)gltang_string_bytes(s);
  const unsigned char * end = p + s->byte_length;
  while (p < end && is_space(*p)) {
    ++p;
  }
  bool negative = false;
  if (p < end && (*p == '+' || *p == '-')) {
    negative = *p == '-';
    ++p;
  }
  if (p >= end || *p < '0' || *p > '9') {
    return ERROR(GLTANG_ERROR_NOT_A_NUMBER);
  }
  uint64_t magnitude = 0;
  bool too_big = false;
  uint64_t limit = negative ? (uint64_t)1 << 63 : ((uint64_t)1 << 63) - 1u;
  for (; p < end && *p >= '0' && *p <= '9'; ++p) {
    uint64_t digit = (uint64_t)(*p - '0');
    if (magnitude > (limit - digit) / 10u) {
      too_big = true;
    }
    else {
      magnitude = magnitude * 10u + digit;
    }
  }
  if (too_big) {
    return overflow(exec, !negative);
  }
  return gltang_vm_make_int(exec, negative ? (int64_t)(0 - magnitude) : (int64_t)magnitude);
}

static GLTANG_Value string_to_float(GLTANG_Execution * exec, GLTANG_Value v) {
  const GLTANG_StringBlock * s = gltang_vm_string(v);
  const char * text = gltang_string_bytes(s);
  // strtod reads the locale's decimal point. The language's is '.', so in a
  // locale that uses another the text is copied with the point swapped.
  const struct lconv * lc = localeconv();
  const char * point = lc && lc->decimal_point ? lc->decimal_point : ".";
  char * copy = NULL;
  if (point[0] && point[1] == '\0' && point[0] != '.' && memchr(text, '.', (size_t)s->byte_length)) {
    copy = gcu_allocator_malloc(exec->allocator, (size_t)s->byte_length + 1u);
    if (!copy) {
      return exec->roots[GLTANG_ROOT_OOM];
    }
    memcpy(copy, text, (size_t)s->byte_length + 1u);
    char * dot = memchr(copy, '.', (size_t)s->byte_length);
    *dot = point[0];
    text = copy;
  }
  char * end = NULL;
  double d = strtod(text, &end);
  bool none = end == text;
  gcu_allocator_free(exec->allocator, copy);
  if (none) {
    return ERROR(GLTANG_ERROR_NOT_A_NUMBER);
  }
  return gltang_vm_make_float(exec, d);
}

static GLTANG_Value float_to_int(GLTANG_Execution * exec, double d) {
  if (isnan(d)) {
    return ERROR(GLTANG_ERROR_NOT_A_NUMBER);
  }
  if (d >= 9223372036854775808.0) {
    return overflow(exec, true);
  }
  if (d < -9223372036854775808.0) {
    return overflow(exec, false);
  }
  return gltang_vm_make_int(exec, (int64_t)d);
}

GLTANG_Value gltang_vm_op_cast(GLTANG_Execution * exec, GLTANG_Value v, GLTANG_Cast_Type type) {
  GLTANG_ValueKind kind = gltang_vm_kind(v);
  if (kind == GLTANG_KIND_ERROR) {
    return ERROR(GLTANG_ERROR_NOT_IMPLEMENTED);
  }
  if (kind == GLTANG_KIND_FUNCTION || kind == GLTANG_KIND_LIBRARY || kind == GLTANG_KIND_RNG) {
    return ERROR(GLTANG_ERROR_NOT_SUPPORTED);
  }
  switch (type) {
    case GLTANG_CAST_TYPE_BOOLEAN:
      return gltang_v_from_bool(gltang_vm_truthy(v));
    case GLTANG_CAST_TYPE_INTEGER:
      switch (kind) {
        case GLTANG_KIND_NULL: return gltang_v_from_small_int(0);
        case GLTANG_KIND_BOOL: return gltang_v_from_small_int(v == GLTANG_V_TRUE ? 1 : 0);
        case GLTANG_KIND_INTEGER: return v;
        case GLTANG_KIND_FLOAT: return float_to_int(exec, gltang_vm_float(v));
        case GLTANG_KIND_STRING: return string_to_int(exec, v);
        default: return ERROR(GLTANG_ERROR_NOT_SUPPORTED);
      }
    case GLTANG_CAST_TYPE_FLOAT:
      switch (kind) {
        case GLTANG_KIND_NULL: return gltang_vm_make_float(exec, 0.0);
        case GLTANG_KIND_BOOL: return gltang_vm_make_float(exec, v == GLTANG_V_TRUE ? 1.0 : 0.0);
        case GLTANG_KIND_INTEGER: return gltang_vm_make_float(exec, (double)gltang_vm_int(v));
        case GLTANG_KIND_FLOAT: return v;
        case GLTANG_KIND_STRING: return string_to_float(exec, v);
        default: return ERROR(GLTANG_ERROR_NOT_SUPPORTED);
      }
    case GLTANG_CAST_TYPE_STRING:
      return gltang_vm_to_string(exec, v);
  }
  return ERROR(GLTANG_ERROR_NOT_SUPPORTED);
}

// ---------------------------------------------------------------------------
// Indexing, attributes, slices
// ---------------------------------------------------------------------------

GLTANG_Value gltang_vm_op_index(GLTANG_Execution * exec, GLTANG_Value container, GLTANG_Value index) {
  switch (gltang_vm_kind(container)) {
    case GLTANG_KIND_ARRAY: {
      if (!is_integer_value(index)) {
        return ERROR(GLTANG_ERROR_INVALID_INDEX);
      }
      int64_t i = gltang_vm_int(index);
      const GLTANG_ArrayObject * array = gltang_vm_array(container);
      int64_t n = (int64_t)array->length;
      if (i < 0) {
        i += n;
      }
      return i < 0 || i >= n ? GLTANG_V_NULL : array->store.typed->slots[i];
    }
    case GLTANG_KIND_STRING: {
      if (!is_integer_value(index)) {
        return ERROR(GLTANG_ERROR_INVALID_INDEX);
      }
      int64_t i = gltang_vm_int(index);
      int64_t n = (int64_t)gltang_vm_string(container)->grapheme_length;
      if (i < 0) {
        i += n;
      }
      return i < 0 ? gltang_vm_string_grapheme(exec, container, (size_t)n) : gltang_vm_string_grapheme(exec, container, (size_t)i);
    }
    case GLTANG_KIND_MAP: {
      if (!gltang_v_is_kind(index, GLTANG_OBJ_STRING)) {
        return ERROR(GLTANG_ERROR_MAP_KEY_NOT_STRING);
      }
      const GLTANG_StringBlock * key = gltang_vm_string(index);
      bool found;
      return gltang_vm_map_get(container, gltang_string_bytes(key), (size_t)key->byte_length, &found);
    }
    default:
      return ERROR(GLTANG_ERROR_NOT_SUPPORTED);
  }
}

static bool name_is(const char * name, size_t length, const char * text) {
  return length == strlen(text) && !memcmp(name, text, length);
}

GLTANG_Value gltang_vm_op_attr(GLTANG_Execution * exec, GLTANG_Value container, GLTANG_Value name_value) {
  const GLTANG_StringBlock * name = gltang_vm_string(name_value);
  return gltang_vm_op_attr_named(exec, container, gltang_string_bytes(name), (size_t)name->byte_length);
}

GLTANG_Value gltang_vm_op_attr_named(GLTANG_Execution * exec, GLTANG_Value container, const char * name, size_t length) {
  switch (gltang_vm_kind(container)) {
    case GLTANG_KIND_STRING:
      if (name_is(name, length, "length")) {
        return gltang_vm_make_int(exec, (int64_t)gltang_vm_string(container)->grapheme_length);
      }
      if (name_is(name, length, "byte_length")) {
        return gltang_vm_make_int(exec, (int64_t)gltang_vm_string(container)->byte_length);
      }
      if (name_is(name, length, "html")) {
        return gltang_vm_string_retag(exec, container, GLTANG_UNICODE_STRING_TYPE_HTML);
      }
      if (name_is(name, length, "html_attribute")) {
        return gltang_vm_string_retag(exec, container, GLTANG_UNICODE_STRING_TYPE_HTML_ATTRIBUTE);
      }
      if (name_is(name, length, "percent")) {
        return gltang_vm_string_retag(exec, container, GLTANG_UNICODE_STRING_TYPE_PERCENT);
      }
      if (name_is(name, length, "javascript")) {
        return gltang_vm_string_retag(exec, container, GLTANG_UNICODE_STRING_TYPE_JAVASCRIPT);
      }
      if (name_is(name, length, "raw")) {
        return gltang_vm_string_retag(exec, container, GLTANG_UNICODE_STRING_TYPE_TRUSTED);
      }
      if (name_is(name, length, "render")) {
        return gltang_vm_string_render(exec, container);
      }
      return ERROR(GLTANG_ERROR_NOT_IMPLEMENTED);
    case GLTANG_KIND_ARRAY:
      if (name_is(name, length, "size")) {
        return gltang_vm_make_int(exec, (int64_t)gltang_vm_array(container)->length);
      }
      return ERROR(GLTANG_ERROR_NOT_IMPLEMENTED);
    case GLTANG_KIND_MAP: {
      // A map has no attributes of its own, so a name is a key; one it does
      // not hold is null (13.7, 13.38).
      bool found;
      return gltang_vm_map_get(container, name, length, &found);
    }
    case GLTANG_KIND_LIBRARY:
      return gltang_vm_library_attr(exec, container, name, length);
    case GLTANG_KIND_RNG:
      return gltang_vm_rng_attr(exec, container, name, length);
    default:
      return ERROR(GLTANG_ERROR_NOT_IMPLEMENTED);
  }
}

/** Python's slice arithmetic for a sequence of `length` (4.9). */
static bool slice_indices(int64_t length, bool has_start, int64_t start, bool has_stop, int64_t stop, int64_t step, int64_t * out_start, int64_t * out_count) {
  if (step == 0) {
    return false;
  }
  bool backwards = step < 0;
  int64_t lower = backwards ? -1 : 0;
  int64_t upper = backwards ? length - 1 : length;
  if (!has_start) {
    start = backwards ? upper : lower;
  }
  else if (start < 0) {
    start += length;
    if (start < lower) {
      start = lower;
    }
  }
  else if (start >= length) {
    start = upper;
  }
  if (!has_stop) {
    stop = backwards ? lower : upper;
  }
  else if (stop < 0) {
    stop += length;
    if (stop < lower) {
      stop = lower;
    }
  }
  else if (stop >= length) {
    stop = upper;
  }
  int64_t count = 0;
  if (backwards) {
    if (stop < start) {
      uint64_t span = (uint64_t)(start - stop - 1);
      uint64_t unit = (uint64_t)0 - (uint64_t)step;
      count = (int64_t)(span / unit + 1u);
    }
  }
  else if (start < stop) {
    uint64_t span = (uint64_t)(stop - start - 1);
    count = (int64_t)(span / (uint64_t)step + 1u);
  }
  *out_start = start;
  *out_count = count;
  return true;
}

GLTANG_Value gltang_vm_op_slice(GLTANG_Execution * exec, GLTANG_Value container, unsigned flags, const GLTANG_Value * parts) {
  // The parts live on the operand stack, which moves at a GC point: read them
  // all before anything allocates.
  GLTANG_Value values[3] = {GLTANG_V_NULL, GLTANG_V_NULL, GLTANG_V_NULL};
  size_t at = 0;
  for (unsigned bit = 0; bit < 3; ++bit) {
    if (flags & (1u << bit)) {
      values[bit] = parts[at++];
    }
  }
  bool is_array = gltang_v_is_kind(container, GLTANG_OBJ_ARRAY);
  bool is_string = gltang_v_is_kind(container, GLTANG_OBJ_STRING);
  if (!is_array && !is_string) {
    return ERROR(GLTANG_ERROR_NOT_SUPPORTED);
  }
  // A part whose value is null is an omitted part, as in ctang, whose parser
  // pushes a null for each part the source leaves out and whose slice takes a
  // null to mean "the default". A variable that holds null is therefore the
  // same as no part, and not an invalid index.
  for (unsigned bit = 0; bit < 3; ++bit) {
    if ((flags & (1u << bit)) && values[bit] == GLTANG_V_NULL) {
      flags &= ~(1u << bit);
    }
  }
  int64_t numbers[3] = {0, 0, 1};
  for (unsigned bit = 0; bit < 3; ++bit) {
    if (flags & (1u << bit)) {
      if (!is_integer_value(values[bit])) {
        return ERROR(GLTANG_ERROR_INVALID_INDEX);
      }
      numbers[bit] = gltang_vm_int(values[bit]);
    }
  }
  int64_t length = is_array ? (int64_t)gltang_vm_array(container)->length : (int64_t)gltang_vm_string(container)->grapheme_length;
  int64_t start, count;
  if (!slice_indices(length, flags & 1u, numbers[0], flags & 2u, numbers[1], numbers[2], &start, &count)) {
    return ERROR(GLTANG_ERROR_INVALID_INDEX);
  }
  if (is_string) {
    return gltang_vm_string_slice(exec, container, start, count, numbers[2]);
  }
  GLTANG_Value result = gltang_vm_array_new(exec, (size_t)count);
  if (!gltang_v_is_kind(result, GLTANG_OBJ_ARRAY)) {
    return result;
  }
  if (!gltang_vm_temp_push(exec, result)) {
    return exec->roots[GLTANG_ROOT_OOM];
  }
  Pacer pacer = GLTANG_PACER(exec, ARRAY_SLICE);
  GLTANG_Value failure = GLTANG_V_NULL;
  GLTANG_Status st = GLTANG_ST_OK;
  int64_t i = start;
  for (int64_t k = 0; k < count && st == GLTANG_ST_OK; ++k) {
    GLTANG_Value element = gltang_vm_array(container)->store.typed->slots[i];
    if (gltang_vm_is_container(element)) {
      GLTANG_Value copy = gltang_vm_deep_copy(exec, element);
      if (copy == GLTANG_V_UNWIND) {
        st = GLTANG_ST_UNWIND;
        failure = GLTANG_V_UNWIND;
        break;
      }
      if (gltang_vm_is_error(copy)) {
        st = GLTANG_ST_OOM;
        failure = copy;
        break;
      }
      element = copy;
    }
    gltang_vm_array_push(exec, result, element);
    if (k + 1 < count) {
      i += numbers[2];
    }
    st = pace(&pacer, 1);
    if (st != GLTANG_ST_OK) {
      failure = GLTANG_V_UNWIND;
    }
  }
  gltang_vm_temp_pop(exec);
  return st == GLTANG_ST_OK ? result : failure;
}

/** Copies `v` if it is a container and the store asked for a copy. */
static GLTANG_Value stored_value(GLTANG_Execution * exec, GLTANG_Value v, bool adopt) {
  return adopt && gltang_vm_is_container(v) ? gltang_vm_deep_copy(exec, v) : v;
}

GLTANG_Value gltang_vm_op_set_index(GLTANG_Execution * exec, GLTANG_Value container, GLTANG_Value index, GLTANG_Value v, bool adopt) {
  switch (gltang_vm_kind(container)) {
    case GLTANG_KIND_ARRAY: {
      if (!is_integer_value(index)) {
        return ERROR(GLTANG_ERROR_INVALID_INDEX);
      }
      int64_t i = gltang_vm_int(index);
      int64_t n = (int64_t)gltang_vm_array(container)->length;
      if (i < 0) {
        i += n;
        if (i < 0) {
          return ERROR(GLTANG_ERROR_INVALID_INDEX);
        }
      }
      if (i >= n) {
        // An index at or past the end grows the array, filling with null
        // (4.13). The slot being assigned is written after the growth.
        GLTANG_Value grown = gltang_vm_array_grow(exec, container, (size_t)i + 1u);
        if (grown != container) {
          return grown;
        }
      }
      GLTANG_Value stored = stored_value(exec, v, adopt);
      if (gltang_vm_is_error(stored) && stored != v) {
        return stored;
      }
      if (stored == GLTANG_V_UNWIND) {
        return stored;
      }
      gltang_vm_array_set(exec, container, (size_t)i, stored);
      return stored;
    }
    case GLTANG_KIND_MAP: {
      if (!gltang_v_is_kind(index, GLTANG_OBJ_STRING)) {
        return ERROR(GLTANG_ERROR_MAP_KEY_NOT_STRING);
      }
      GLTANG_Value stored = stored_value(exec, v, adopt);
      if (stored == GLTANG_V_UNWIND || (gltang_vm_is_error(stored) && stored != v)) {
        return stored;
      }
      GLTANG_Value r = gltang_vm_map_set(exec, container, index, stored);
      return r == container ? stored : r;
    }
    default:
      return ERROR(GLTANG_ERROR_NOT_SUPPORTED);
  }
}

GLTANG_Value gltang_vm_op_set_attr(GLTANG_Execution * exec, GLTANG_Value container, GLTANG_Value name, GLTANG_Value v, bool adopt) {
  switch (gltang_vm_kind(container)) {
    case GLTANG_KIND_MAP: {
      GLTANG_Value stored = stored_value(exec, v, adopt);
      if (stored == GLTANG_V_UNWIND || (gltang_vm_is_error(stored) && stored != v)) {
        return stored;
      }
      GLTANG_Value r = gltang_vm_map_set(exec, container, name, stored);
      return r == container ? stored : r;
    }
    case GLTANG_KIND_ARRAY:
      // The error subscripting an array with a string would be.
      return ERROR(GLTANG_ERROR_INVALID_INDEX);
    default:
      return ERROR(GLTANG_ERROR_NOT_SUPPORTED);
  }
}
