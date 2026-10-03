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
 * @file lang-tang.h
 * @stability stable
 *
 * Umbrella header for Ghoti.io Lang-tang.
 *
 * The Tang engine of the language runtime stack. At this stage it holds the
 * front end: the parser and syntax tree ported from ctang, and the interface
 * that parses a template or a script into an owned tree. Execution arrives
 * with the interpreter. The syntax tree's node classes are in the `ast/`
 * headers, labelled `free`, and are deliberately not included here.
 */

#ifndef GHOTI_IO_GLTANG_LANG_TANG_H
#define GHOTI_IO_GLTANG_LANG_TANG_H

#include <ghoti.io/lang-tang/macros.h>

#include <ghoti.io/lang-tang/allocator.h>
#include <ghoti.io/lang-tang/core.h>
#include <ghoti.io/lang-tang/libver.h>
#include <ghoti.io/lang-tang/parse.h>

#endif /* GHOTI_IO_GLTANG_LANG_TANG_H */
