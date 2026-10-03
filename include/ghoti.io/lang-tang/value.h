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
 * @file value.h
 * @stability free
 *
 * Values, as a host sees them: the kinds, the errors, the heap codec and the
 * plain-data value a host hands in.
 *
 * Inside the engine a value is one 64-bit word (see documentation/design.md for
 * the encoding and the alternatives rejected). A host never reads that word.
 * It reads a result through the accessors in execution.h, which give a kind
 * and then an integer, a float, a boolean or text, and it describes its own
 * values to the engine as ::GLTANG_HostValue, which is plain data.
 *
 * This header is `free`: the stable host API that wraps it is not part of it.
 */

#ifndef GHOTI_IO_GLTANG_VALUE_H
#define GHOTI_IO_GLTANG_VALUE_H

#include <ghoti.io/lang-tang/macros.h>

#include <ghoti.io/lang-tang/core.h>
#include <ghoti.io/lang-tang/unicodeString.h>
#include <ghoti.io/runtime-heap/options.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief What kind of value a result is. */
typedef enum {
  GLTANG_KIND_NULL = 0, ///< `null`, and the result of an empty program.
  GLTANG_KIND_BOOL,     ///< `true` or `false`.
  GLTANG_KIND_INTEGER,  ///< A signed 64-bit integer.
  GLTANG_KIND_FLOAT,    ///< An IEEE-754 double.
  GLTANG_KIND_STRING,   ///< A grapheme string with segment-tagged encodings.
  GLTANG_KIND_ARRAY,    ///< A mutable array.
  GLTANG_KIND_MAP,      ///< A mutable map from strings to values.
  GLTANG_KIND_FUNCTION, ///< A function declared by the program.
  GLTANG_KIND_ERROR     ///< An error value (language reference, section 10.2).
} GLTANG_ValueKind;

/**
 * @brief Which error an error value is.
 *
 * Errors are values: they are falsy, print as nothing (a marker prints as its
 * marker), and never unwind a statement. Appended to, never renumbered.
 */
typedef enum {
  GLTANG_ERROR_DIVIDE_BY_ZERO = 0,     ///< `/` with a zero divisor.
  GLTANG_ERROR_MODULO_BY_ZERO,         ///< `%` with a zero divisor.
  GLTANG_ERROR_NOT_SUPPORTED,          ///< An operation the operand types do not define.
  GLTANG_ERROR_NOT_IMPLEMENTED,        ///< Defined, but not yet written.
  GLTANG_ERROR_INVALID_INDEX,          ///< A non-integer index, a zero step, a negative index past the start.
  GLTANG_ERROR_MAP_KEY_NOT_STRING,     ///< A map indexed with a non-string.
  GLTANG_ERROR_INVALID_FUNCTION_CALL,  ///< Calling something that is not a function.
  GLTANG_ERROR_ARGUMENT_COUNT_MISMATCH, ///< Calling with the wrong number of arguments.
  GLTANG_ERROR_RECURSION_LIMIT,        ///< A call past the guest-depth budget.
  GLTANG_ERROR_OUT_OF_MEMORY,          ///< An allocation failed.
  GLTANG_ERROR_INTEGER_TOO_LARGE,      ///< A marker: prints as `[INTEGER TOO LARGE]`.
  GLTANG_ERROR_INTEGER_TOO_SMALL,      ///< A marker: prints as `[INTEGER TOO SMALL]`.
  GLTANG_ERROR_NOT_A_NUMBER,           ///< A marker: prints as `[NOT A NUMBER]`.
  GLTANG_ERROR_KIND_COUNT              ///< Not an error: closes the enum.
} GLTANG_ErrorKind;

/**
 * @brief The text of an error: `Divide by zero`, or `[INTEGER TOO LARGE]`.
 *
 * @param kind An error kind.
 * @return A static string; "Unknown error" for a value outside the enum.
 */
GLTANG_API const char * gltang_error_kind_message(GLTANG_ErrorKind kind);

/**
 * @brief Whether an error prints as its own text rather than as nothing.
 *
 * @param kind An error kind.
 * @return True for the three markers.
 */
GLTANG_API bool gltang_error_kind_is_marker(GLTANG_ErrorKind kind);

/**
 * @brief Where an error value was created.
 *
 * Every error records this when the operation that fails creates it, and the
 * host reads it back. The poll identity is the same pair a poll uses, so a
 * debugger or an error list can name the place in the same terms.
 */
typedef struct GLTANG_ErrorOrigin {
  const char * file;    ///< The program's file name; NULL if the error predates the program.
  int line;             ///< The 1-based source line; 0 if unknown.
  uint64_t function;    ///< The function index of the poll identity.
  uint64_t offset;      ///< The bytecode offset of the poll identity.
} GLTANG_ErrorOrigin;

/** @brief The kinds of value a host can describe in plain data. */
typedef enum {
  GLTANG_HOST_NULL = 0, ///< `null`.
  GLTANG_HOST_BOOL,     ///< A boolean.
  GLTANG_HOST_INTEGER,  ///< An integer.
  GLTANG_HOST_FLOAT,    ///< A float.
  GLTANG_HOST_STRING    ///< A string.
} GLTANG_HostKind;

/**
 * @brief A value a host hands to the engine, as plain data.
 *
 * The engine copies it into the heap, so nothing here outlives the call.
 */
typedef struct GLTANG_HostValue {
  GLTANG_HostKind kind;         ///< Which member below is read.
  bool boolean;                 ///< ::GLTANG_HOST_BOOL.
  int64_t integer;              ///< ::GLTANG_HOST_INTEGER.
  double number;                ///< ::GLTANG_HOST_FLOAT.
  const char * text;            ///< ::GLTANG_HOST_STRING: UTF-8, `length` bytes.
  size_t length;                ///< ::GLTANG_HOST_STRING: the byte count.
  GLTANG_String_Type encoding;  ///< ::GLTANG_HOST_STRING: the segment's tag.
} GLTANG_HostValue;

/**
 * @brief Describes this engine's value encoding to the collector.
 *
 * A heap is created by the host before the engine attaches (AD-20), so the
 * host has to tell the heap how to read a word. Pass the same codec to
 * ::grheap_options_set_value_codec, or call ::gltang_heap_options_configure.
 * A heap left on the identity codec still works, because a pointer is its own
 * word, but it counts every integer and boolean as an invalid pointer.
 *
 * @param out_codec Receives the codec. NULL is ignored.
 */
GLTANG_API void gltang_value_codec(GRHEAP_ValueCodec * out_codec);

/**
 * @brief Sets the heap options an engine needs: its value codec.
 *
 * @param options The heap options, from ::grheap_options_create.
 * @return ::GLTANG_OK, or ::GLTANG_ERR_INVALID for NULL.
 */
GLTANG_API GLTANG_Result gltang_heap_options_configure(GRHEAP_Options * options);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GLTANG_VALUE_H */
