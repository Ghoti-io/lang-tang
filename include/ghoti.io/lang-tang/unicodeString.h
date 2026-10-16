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
 * @stability free
 *
 * Unicode String class for use by the Tang language library.
 *
 * Strings are stored as UTF-8 encoded byte arrays.  Strings exist to be used
 * as output from the Tang Language library, and may need to be encoded as
 * percent-encoded strings, html entities, or other formats.  As such, the
 * GLTANG_Unicode_String object is used to represent a string that is both Unicode
 * aware and can be encoded in a variety of formats, a property which is
 * retained even through concatenation or substring operations.
 *
 * Furthermore, the GLTANG_Unicode_String object is grapheme-aware, meaning that
 * it can be used to extract substrings based on grapheme positions, rather
 * than byte positions.  This is important for languages such as Thai, where
 * a single character may be represented by multiple bytes, or for emoji, where
 * a single character may be represented by multiple code points.  As an
 * extreme example, the flag of Scotland is represented by 1 grapheme, which is
 * 7 code points, and requires 28 bytes when encoded in UTF-8.  These bytes or
 * code points should not be split up when extracting substrings, so all string
 * operations are grapheme-aware.
 */

#ifndef GHOTI_IO_GLTANG_UNICODESTRING_H
#define GHOTI_IO_GLTANG_UNICODESTRING_H

#include <ghoti.io/lang-tang/macros.h>
#include <ghoti.io/lang-tang/encoding.h>

#include <stddef.h>
#include <ghoti.io/cutil/vector.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Get the top 32 bits of a 64-bit integer.
 *
 * @param x The 64-bit integer.
 * @return The top 32 bits of the integer.
 */
#define GLTANG_UC_GET_TYPE_FROM_TYPE_OFFSET_PAIR(x) ((x.ui64) >> 32)

/**
 * Get the bottom 32 bits of a 64-bit integer.
 *
 * @param x The 64-bit integer.
 * @return The bottom 32 bits of the integer.
 */
#define GLTANG_UC_GET_OFFSET_FROM_TYPE_OFFSET_PAIR(x) ((x.ui64) & 0xFFFFFFFF)

/**
 * Make a 64-bit integer from two 32-bit integers.
 *
 * @param top The top 32 bits of the integer.
 * @param bot The bottom 32 bits of the integer.
 * @return The 64-bit integer.
 */
#define GLTANG_UC_MAKE_TYPE_OFFSET_PAIR(top, bot) GCU_TYPE64_UI64(((uint64_t)(top) << 32) | (uint64_t)(bot))


typedef struct GLTANG_Unicode_String GLTANG_Unicode_String;

/**
 * Basic Unicode String object.
 *
 * The vectors should be considered as internal to the object, and should not
 * be accessed directly.  Instead, use the functions provided.  They are
 * included here for efficiency in locality of reference when accessing the
 * string.
 *
 * A Unicode String object is a UTF-8 encoded string that is grapheme-aware.
 * It also tracks the type of the string in parts, so that the string can be
 * encoded correctly, even if it has been concatenated with other strings.  The
 * concatenation and substring functions will ensure that the type of the
 * string is maintained correctly as the string is manipulated.
 */
struct GLTANG_Unicode_String {
  const char * buffer;             ///< The string buffer.
  size_t grapheme_length;          ///< Length of the string in graphemes.
  size_t byte_length;              ///< Length of the string in bytes.  Does
                                   ///<   not include the null terminator.
  GCU_Vector32 * grapheme_offsets; ///< Mapping of grapheme to the index where
                                   ///<   the grapheme starts.  Will contain
                                   ///<   grapheme_length + 1 entries.
  GCU_Vector64 * string_type;      ///< Tracks the type of string in parts, so
                                   ///<   that the string can be encoded
                                   ///<   correctly, even if it has been
                                   ///<   concatenated with other strings.
                                   ///<   The type is the upper 32 bits of the
                                   ///<   64-bit integer, and the offset is the
                                   ///<   lower 32 bits.  The offset is the
                                   ///<   grapheme offset.
};

/**
 * A rendered string: bytes the caller owns (free with gcu_free).
 */
typedef struct GLTANG_Unicode_Rendered_String {
  char * buffer;                   ///< The rendered bytes, null-terminated; NULL on failure.
  size_t length;                   ///< Length in bytes, not counting the terminator.
} GLTANG_Unicode_Rendered_String;

/**
 * Construct a new Unicode String object and copy the source string buffer.
 *
 * @param source The source string.  It is not adopted.
 * @param length The length of the source string in bytes (not including the
 *   null terminator).
 * @param type The type of string being created.
 * @return A pointer to the Unicode String object, or NULL if there was an error.
 */
GLTANG_API GLTANG_NO_DISCARD GLTANG_Unicode_String * gltang_unicode_string_create(const char * source, size_t length, GLTANG_String_Type type);

/**
 * Construct a new Unicode String object and adopt ownership of the source
 * string buffer.
 *
 * @param source The source string.  It is adopted.
 * @param length The length of the source string in bytes (not including the
 *   null terminator).
 * @param type The type of string being created.
 * @return A pointer to the Unicode String object, or NULL if there was an error.
 */
GLTANG_API GLTANG_NO_DISCARD GLTANG_Unicode_String * gltang_unicode_string_create_and_adopt(const char * source, size_t length, GLTANG_String_Type type);

/**
 * Destroy a Unicode String object.
 * @param string The string to destroy.
 */
GLTANG_API void gltang_unicode_string_destroy(GLTANG_Unicode_String * string);

/**
 * Concatenate two Unicode Strings.
 *
 * Graphemes are found again across the join (a combining mark at the start of
 * the second string joins the last grapheme of the first), as the language's
 * own `+` does, and each grapheme keeps the tag of the run it began in. Runs of
 * equal tag join into one. The inputs are not adopted and not changed.
 *
 * @param string1 The first string.
 * @param string2 The second string.
 * @return The new string, or NULL on an allocation failure or a NULL argument.
 */
GLTANG_API GLTANG_NO_DISCARD GLTANG_Unicode_String * gltang_unicode_string_concat(const GLTANG_Unicode_String * string1, const GLTANG_Unicode_String * string2);

/**
 * The substring of a Unicode String, by graphemes.
 *
 * A start at or past the end, or a count of zero, gives an empty trusted
 * string. The substring keeps the tag of every grapheme it takes.
 *
 * @param string The string.
 * @param grapheme_start The first grapheme.
 * @param grapheme_count How many graphemes (clamped to the end).
 * @return The new string, or NULL on an allocation failure or a NULL string.
 */
GLTANG_API GLTANG_NO_DISCARD GLTANG_Unicode_String * gltang_unicode_string_substring(const GLTANG_Unicode_String * string, size_t grapheme_start, size_t grapheme_count);

/**
 * Render a string, encoding each run by its tag: trusted as it is, html and
 * html-attribute HTML-escaped (the attribute tag also escapes quotes),
 * percent percent-encoded, JavaScript escaped. The bytes are the ones the
 * engine's own output render gives for the same tagged text.
 *
 * @param string The string.
 * @return The rendered bytes (free `buffer` with gcu_free), or a NULL buffer on
 *   an allocation failure or a NULL string.
 */
GLTANG_API GLTANG_NO_DISCARD GLTANG_Unicode_Rendered_String gltang_unicode_string_render(const GLTANG_Unicode_String * string);

/**
 * Encode text as ::GLTANG_UNICODE_STRING_TYPE_HTML.
 *
 * @param source The text.
 * @param length Its length in bytes, not counting any terminator.
 * @return The encoded bytes (free `buffer` with gcu_free), or a NULL buffer on
 *   an allocation failure.
 */
GLTANG_API GLTANG_NO_DISCARD GLTANG_Unicode_Rendered_String gltang_unicode_string_html_encode(const char * source, size_t length);

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // GHOTI_IO_GLTANG_UNICODESTRING_H
