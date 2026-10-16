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
 * @file encoding.h
 * @stability stable
 *
 * The encoding tag a string segment carries. It is its own header so that the
 * host-value API (description.h), which is stable, can name it without
 * including unicodeString.h, which is free.
 */

#ifndef GHOTI_IO_GLTANG_ENCODING_H
#define GHOTI_IO_GLTANG_ENCODING_H

#include <ghoti.io/lang-tang/macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The type of the substring.
 *
 * This allows a string to be manipulated in its original form, even if it will
 * later be encoded.  For example, a string may be "Untrusted", which will
 * result in a percent or html encoding being applied (depending on the
 * context), but it may be concatenated with a "Trusted" string, which will not
 * be encoded.  The resulting string will then "tag" the "Trusted" and
 * "Untrusted" parts, so that they can be encoded correctly, and yet the string
 * may be manipulated as a single string for further concatenation or
 * substring.
 */
typedef enum {
  GLTANG_UNICODE_STRING_TYPE_TRUSTED,   ///< String is from a trusted source.
  GLTANG_UNICODE_STRING_TYPE_HTML,      ///< String is to be HTML encoded. Only
                                     ///<   characters that need to be encoded
                                     ///<   in order to prevent injection
                                     ///<   attacks will actually be encoded.
  GLTANG_UNICODE_STRING_TYPE_HTML_ATTRIBUTE, ///< String is to be HTML encoded,
                                     ///<   with all more aggressive encoding
                                     ///<   applied. This is used for attribute
                                     ///<   values.
  GLTANG_UNICODE_STRING_TYPE_PERCENT,   ///< String is to be percent encoded.
                                     ///<   This is used for query strings.
  GLTANG_UNICODE_STRING_TYPE_JAVASCRIPT,///< String is to be JavaScript encoded.
                                     ///<   This is used for inline scripts.
} GLTANG_String_Type;

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GLTANG_ENCODING_H */
