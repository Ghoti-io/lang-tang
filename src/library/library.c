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
 * @file
 *
 * The library registry a host builds: create, add members, seal, share.
 */

#include <ghoti.io/lang-tang/macros.h>

#include <string.h>
#include <ghoti.io/cutil/memory.h>
#include "library_internal.h"

static char * copy_text(const char * text, size_t length) {
  char * copy = length == SIZE_MAX ? NULL : gcu_malloc(length + 1u);
  if (!copy) {
    return NULL;
  }
  if (length) {
    memcpy(copy, text, length);
  }
  copy[length] = '\0';
  return copy;
}

static bool valid_name(const char * name) {
  return name && name[0] && !strchr(name, '.');
}

GLTANG_Result gltang_library_create(const char * name, GLTANG_Library ** out_library) {
  if (!out_library || (name && !valid_name(name))) {
    return GLTANG_ERR_INVALID;
  }
  GLTANG_Library * library = gcu_calloc(1, sizeof(GLTANG_Library));
  if (!library) {
    return GLTANG_ERR_OOM;
  }
  if (name) {
    library->name = copy_text(name, strlen(name));
    if (!library->name) {
      gcu_free(library);
      return GLTANG_ERR_OOM;
    }
  }
  atomic_init(&library->references, 1);
  atomic_init(&library->sealed, false);
  *out_library = library;
  return GLTANG_OK;
}

GLTANG_Library * gltang_library_retain(GLTANG_Library * library) {
  if (library && !library->is_static) {
    atomic_fetch_add_explicit(&library->references, 1, memory_order_relaxed);
  }
  return library;
}

static void member_free(GLTANG_LibraryMember * member) {
  gcu_free(member->name);
  gcu_free(member->text);
  gltang_program_release(member->program);
  gltang_library_release(member->library);
}

static void library_free(GLTANG_Library * library) {
  for (size_t i = 0; i < library->count; ++i) {
    member_free(&library->members[i]);
  }
  gcu_free(library->members);
  gcu_free(library->name);
  gcu_free(library);
}

void gltang_library_release(GLTANG_Library * library) {
  if (!library || library->is_static) {
    return;
  }
  // The last reference frees. acq_rel so that every other thread's reads of the
  // library happen before the free.
  if (atomic_fetch_sub_explicit(&library->references, 1, memory_order_acq_rel) == 1) {
    library_free(library);
  }
}

bool gltang_library_sealed(const GLTANG_Library * library) {
  return library && atomic_load_explicit(&((GLTANG_Library *)(uintptr_t)library)->sealed, memory_order_acquire);
}

const char * gltang_library_name(const GLTANG_Library * library) {
  return library ? library->name : NULL;
}

size_t gltang_library_count(const GLTANG_Library * library) {
  return library ? library->count : 0;
}

GLTANG_Library * gltang_library_attach(GLTANG_Library * library) {
  if (!library) {
    return NULL;
  }
  atomic_store_explicit(&library->sealed, true, memory_order_release);
  return gltang_library_retain(library);
}

const GLTANG_LibraryMember * gltang_library_find(const GLTANG_Library * library, const char * name, size_t length) {
  if (!library) {
    return NULL;
  }
  for (size_t i = 0; i < library->count; ++i) {
    const char * member = library->members[i].name;
    if (!strncmp(member, name, length) && member[length] == '\0') {
      return &library->members[i];
    }
  }
  return NULL;
}

/**
 * Makes room for a member and fills in its name; the caller fills in the rest.
 * Returns NULL, having changed nothing, on a refusal (`*result` says which).
 */
static GLTANG_LibraryMember * member_begin(GLTANG_Library * library, const char * name, GLTANG_Result * result) {
  if (!library || !valid_name(name) || gltang_library_sealed(library) || gltang_library_find(library, name, strlen(name))) {
    *result = GLTANG_ERR_INVALID;
    return NULL;
  }
  if (library->count == library->capacity) {
    size_t capacity = library->capacity ? library->capacity * 2u : 8u;
    GLTANG_LibraryMember * grown = gcu_realloc(library->members, capacity * sizeof(GLTANG_LibraryMember));
    if (!grown) {
      *result = GLTANG_ERR_OOM;
      return NULL;
    }
    library->members = grown;
    library->capacity = capacity;
  }
  char * copy = copy_text(name, strlen(name));
  if (!copy) {
    *result = GLTANG_ERR_OOM;
    return NULL;
  }
  GLTANG_LibraryMember * member = &library->members[library->count];
  memset(member, 0, sizeof(*member));
  member->name = copy;
  *result = GLTANG_OK;
  return member;
}

static GLTANG_Result member_commit(GLTANG_Library * library) {
  ++library->count;
  return GLTANG_OK;
}

GLTANG_Result gltang_library_add_null(GLTANG_Library * library, const char * name) {
  GLTANG_Result r;
  GLTANG_LibraryMember * member = member_begin(library, name, &r);
  if (!member) {
    return r;
  }
  member->kind = GLTANG_MEMBER_NULL;
  return member_commit(library);
}

GLTANG_Result gltang_library_add_bool(GLTANG_Library * library, const char * name, bool value) {
  GLTANG_Result r;
  GLTANG_LibraryMember * member = member_begin(library, name, &r);
  if (!member) {
    return r;
  }
  member->kind = GLTANG_MEMBER_BOOL;
  member->boolean = value;
  return member_commit(library);
}

GLTANG_Result gltang_library_add_integer(GLTANG_Library * library, const char * name, int64_t value) {
  GLTANG_Result r;
  GLTANG_LibraryMember * member = member_begin(library, name, &r);
  if (!member) {
    return r;
  }
  member->kind = GLTANG_MEMBER_INTEGER;
  member->integer = value;
  return member_commit(library);
}

GLTANG_Result gltang_library_add_float(GLTANG_Library * library, const char * name, double value) {
  GLTANG_Result r;
  GLTANG_LibraryMember * member = member_begin(library, name, &r);
  if (!member) {
    return r;
  }
  member->kind = GLTANG_MEMBER_FLOAT;
  member->number = value;
  return member_commit(library);
}

GLTANG_Result gltang_library_add_string(GLTANG_Library * library, const char * name, const char * text, size_t length, GLTANG_String_Type encoding) {
  if (!text && length) {
    return GLTANG_ERR_INVALID;
  }
  if ((unsigned)encoding > (unsigned)GLTANG_UNICODE_STRING_TYPE_JAVASCRIPT) {
    return GLTANG_ERR_INVALID;
  }
  GLTANG_Result r;
  GLTANG_LibraryMember * member = member_begin(library, name, &r);
  if (!member) {
    return r;
  }
  member->text = copy_text(text ? text : "", length);
  if (!member->text) {
    gcu_free(member->name);
    return GLTANG_ERR_OOM;
  }
  member->kind = GLTANG_MEMBER_STRING;
  member->length = length;
  member->encoding = encoding;
  return member_commit(library);
}

GLTANG_Result gltang_library_add_native(GLTANG_Library * library, const char * name, GLTANG_NativeFn function, void * user) {
  if (!function) {
    return GLTANG_ERR_INVALID;
  }
  GLTANG_Result r;
  GLTANG_LibraryMember * member = member_begin(library, name, &r);
  if (!member) {
    return r;
  }
  member->kind = GLTANG_MEMBER_NATIVE;
  member->native = function;
  member->user = user;
  return member_commit(library);
}

GLTANG_Result gltang_library_add_factory(GLTANG_Library * library, const char * name, GLTANG_FactoryFn factory, void * user) {
  if (!factory) {
    return GLTANG_ERR_INVALID;
  }
  GLTANG_Result r;
  GLTANG_LibraryMember * member = member_begin(library, name, &r);
  if (!member) {
    return r;
  }
  member->kind = GLTANG_MEMBER_FACTORY;
  member->factory = factory;
  member->user = user;
  return member_commit(library);
}

GLTANG_Result gltang_library_add_template(GLTANG_Library * library, const char * name, GLTANG_Program * program, uint64_t scope_fuel, GLTANG_ScopePolicy policy) {
  if (!program || (unsigned)policy > (unsigned)GLTANG_SCOPE_PAUSE) {
    return GLTANG_ERR_INVALID;
  }
  GLTANG_Result r;
  GLTANG_LibraryMember * member = member_begin(library, name, &r);
  if (!member) {
    return r;
  }
  member->kind = GLTANG_MEMBER_TEMPLATE;
  member->program = gltang_program_retain(program);
  member->scope_fuel = scope_fuel;
  member->policy = policy;
  return member_commit(library);
}

GLTANG_Result gltang_library_add_library(GLTANG_Library * library, GLTANG_Library * child) {
  if (!child || child == library || !child->name) {
    return GLTANG_ERR_INVALID;
  }
  GLTANG_Result r;
  GLTANG_LibraryMember * member = member_begin(library, child->name, &r);
  if (!member) {
    return r;
  }
  member->kind = GLTANG_MEMBER_LIBRARY;
  member->library = gltang_library_attach(child);
  return member_commit(library);
}
