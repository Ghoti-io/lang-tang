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
 * Context snapshots, the engine's part (CAP-11; AD-12, AD-19, AD-20).
 *
 * runtime-core keeps a snapshot as one blob per keyed registration and
 * runtime-heap writes the heap and the guest stack writes the frames; what is
 * left is what only the engine knows: the execution's state, its output so far,
 * its error list, and the host values (a library, a native function, a
 * template) that the objects in the heap point at.
 *
 * **Host values are names.** A library object holds a pointer to a library, a
 * native or a template object holds one to a library member. Neither can be in
 * a snapshot. Each is written as the library's name and the member's name, and
 * resolved again at the destination in the layers a `use` searches (the
 * execution's libraries, the main program's, the built-ins). The take checks
 * that the name finds the same object in the source, and refuses if it does not,
 * so a name that is ambiguous cannot be restored to the wrong thing.
 *
 * **The execution's own blob.** The state, the main program's identity (its
 * function, constant and global counts and a hash of its content), the number of
 * root slots, the pause location, the output buffer, the error list with its
 * strings, and the templates that have run in this execution (which a later
 * call would need): each of those programs is named the way a host value is.
 * The values themselves (the roots, the temporaries, the constants caches, the
 * guest frames' slots) are not in the blob: they are in the heap image, which
 * writes them back at settle, once this blob has made the slots.
 *
 * **Restore** has two passes over the blob. CHECK parses it and compares it
 * with the destination (a `NEW` execution of the same program, with its
 * libraries attached) and changes nothing. APPLY allocates everything first and
 * installs it last, so a failure leaves nothing behind; ABANDON puts the
 * execution back to what it was when it was created.
 */

#include <ghoti.io/lang-tang/macros.h>

#include <string.h>
#include <ghoti.io/cutil/memory.h>
#include <ghoti.io/runtime-core/b/snapshot.h>
#include <ghoti.io/runtime-heap/image.h>
#include "vm_internal.h"

#define XBLOB_MAGIC UINT64_C(0x3158474E41544C47) /* "GLTANGX1" */

// ---------------------------------------------------------------------------
// Names of host values
// ---------------------------------------------------------------------------
//
// A library is named by where it is: the layer a `use` finds it in (the
// execution's libraries, the main program's, the built-ins) and the names of
// the members, each a library, that lead to it from that layer's root. The root
// of a layer is usually unnamed (the host's own library of natives and
// templates), so a name alone would not do. A member is named by its library
// and its own name. The take checks that the path leads back to the object; a
// path is unique by construction (a library's member names are), so what it
// cannot catch is only a destination that has something else under the same
// names, which the resolution of a member by kind and the identity check of a
// program (for a template) refuse.

#define MAX_LIBRARY_DEPTH 32

typedef struct LibraryPath {
  uint64_t layer;
  uint64_t depth;
  const char * names[MAX_LIBRARY_DEPTH];
} LibraryPath;

/** The three layers a `use` searches, in order. */
static const GLTANG_Library * layer(const GLTANG_Execution * exec, uint64_t index) {
  switch (index) {
    case 0: return exec->libraries;
    case 1: return exec->program_count ? exec->programs[0].program->libraries : NULL;
    default: return gltang_library_builtins();
  }
}

static bool path_down(const GLTANG_Library * library, const GLTANG_Library * target, LibraryPath * path) {
  if (library == target) {
    return true;
  }
  if (!library || path->depth >= MAX_LIBRARY_DEPTH) {
    return false;
  }
  for (size_t i = 0; i < library->count; ++i) {
    const GLTANG_LibraryMember * member = &library->members[i];
    if (member->kind == GLTANG_MEMBER_LIBRARY && member->library) {
      path->names[path->depth++] = member->name;
      if (path_down(member->library, target, path)) {
        return true;
      }
      --path->depth;
    }
  }
  return false;
}

/** Where `target` is, in the first layer that has it. */
static bool locate_library(const GLTANG_Execution * exec, const GLTANG_Library * target, LibraryPath * path) {
  for (uint64_t i = 0; i < 3; ++i) {
    path->layer = i;
    path->depth = 0;
    if (target && layer(exec, i) && path_down(layer(exec, i), target, path)) {
      return true;
    }
  }
  return false;
}

/** The library at the end of a path in the destination's layers, or NULL. */
static const GLTANG_Library * walk_path(const GLTANG_Execution * exec, const LibraryPath * path) {
  if (path->layer > 2 || path->depth > MAX_LIBRARY_DEPTH) {
    return NULL;
  }
  const GLTANG_Library * library = layer(exec, path->layer);
  for (uint64_t i = 0; library && i < path->depth; ++i) {
    const GLTANG_LibraryMember * member = path->names[i] ? gltang_library_find(library, path->names[i], strlen(path->names[i])) : NULL;
    library = member && member->kind == GLTANG_MEMBER_LIBRARY ? member->library : NULL;
  }
  return library;
}

static const GLTANG_Library * owner_down(const GLTANG_Library * library, const GLTANG_LibraryMember * member, int depth) {
  if (!library || depth > MAX_LIBRARY_DEPTH) {
    return NULL;
  }
  if (member >= library->members && member < library->members + library->count) {
    return library;
  }
  for (size_t i = 0; i < library->count; ++i) {
    const GLTANG_LibraryMember * m = &library->members[i];
    if (m->kind == GLTANG_MEMBER_LIBRARY) {
      const GLTANG_Library * found = owner_down(m->library, member, depth + 1);
      if (found) {
        return found;
      }
    }
  }
  return NULL;
}

/** The library `member` belongs to, in any layer. */
static const GLTANG_Library * find_owner(const GLTANG_Execution * exec, const GLTANG_LibraryMember * member) {
  for (uint64_t i = 0; i < 3; ++i) {
    const GLTANG_Library * found = owner_down(layer(exec, i), member, 0);
    if (found) {
      return found;
    }
  }
  return NULL;
}

static bool template_down(const GLTANG_Library * library, const GLTANG_Program * program, const GLTANG_LibraryMember ** out, int depth) {
  if (!library || depth > MAX_LIBRARY_DEPTH) {
    return false;
  }
  for (size_t i = 0; i < library->count; ++i) {
    const GLTANG_LibraryMember * m = &library->members[i];
    if (m->kind == GLTANG_MEMBER_TEMPLATE && m->program == program) {
      *out = m;
      return true;
    }
    if (m->kind == GLTANG_MEMBER_LIBRARY && template_down(m->library, program, out, depth + 1)) {
      return true;
    }
  }
  return false;
}

/** The template member whose program is `program`, in any layer. */
static const GLTANG_LibraryMember * find_template(const GLTANG_Execution * exec, const GLTANG_Program * program) {
  const GLTANG_LibraryMember * member = NULL;
  for (uint64_t i = 0; i < 3; ++i) {
    if (template_down(layer(exec, i), program, &member, 0)) {
      return member;
    }
  }
  return NULL;
}

/** The path of a library, checked to lead back to it. */
static bool library_path(const GLTANG_Execution * exec, const GLTANG_Library * library, LibraryPath * path) {
  return locate_library(exec, library, path) && walk_path(exec, path) == library;
}

/** The path of a member's library and the member's name, checked both ways. */
static bool member_path(const GLTANG_Execution * exec, const GLTANG_LibraryMember * member, LibraryPath * path, const char ** name) {
  const GLTANG_Library * owner = find_owner(exec, member);
  if (!owner || !member->name || !library_path(exec, owner, path) ||
      gltang_library_find(owner, member->name, strlen(member->name)) != member) {
    return false;
  }
  *name = member->name;
  return true;
}

// The same two reads and writes for the two writers (the context snapshot's
// blob and the heap image's extra bytes), which have different types.

static GRCORE_Result core_put_path(GRCORE_SnapshotWriter * w, const LibraryPath * path) {
  GRCORE_Result r = grcore_snapshot_writer_u64(w, path->layer);
  if (r == GRCORE_OK) {
    r = grcore_snapshot_writer_u64(w, path->depth);
  }
  for (uint64_t i = 0; r == GRCORE_OK && i < path->depth; ++i) {
    r = grcore_snapshot_writer_string(w, path->names[i]);
  }
  return r;
}

static GRCORE_Result core_get_path(GRCORE_SnapshotReader * r, LibraryPath * path) {
  GRCORE_Result res = grcore_snapshot_reader_u64(r, &path->layer);
  if (res == GRCORE_OK) {
    res = grcore_snapshot_reader_u64(r, &path->depth);
  }
  if (res == GRCORE_OK && (path->layer > 2 || path->depth > MAX_LIBRARY_DEPTH)) {
    res = GRCORE_ERR_CORRUPT;
  }
  for (uint64_t i = 0; res == GRCORE_OK && i < path->depth; ++i) {
    res = grcore_snapshot_reader_string(r, &path->names[i], NULL);
  }
  return res;
}

static GRHEAP_Result image_put_path(GRHEAP_ImageWriter * w, const LibraryPath * path) {
  GRHEAP_Result r = grheap_image_writer_write(w, &path->layer, sizeof path->layer);
  if (r == GRHEAP_OK) {
    r = grheap_image_writer_write(w, &path->depth, sizeof path->depth);
  }
  for (uint64_t i = 0; r == GRHEAP_OK && i < path->depth; ++i) {
    r = grheap_image_writer_string(w, path->names[i]);
  }
  return r;
}

static GRHEAP_Result image_get_path(GRHEAP_ImageReader * r, LibraryPath * path) {
  GRHEAP_Result res = grheap_image_reader_read(r, &path->layer, sizeof path->layer);
  if (res == GRHEAP_OK) {
    res = grheap_image_reader_read(r, &path->depth, sizeof path->depth);
  }
  if (res == GRHEAP_OK && (path->layer > 2 || path->depth > MAX_LIBRARY_DEPTH)) {
    res = GRHEAP_ERR_CORRUPT;
  }
  for (uint64_t i = 0; res == GRHEAP_OK && i < path->depth; ++i) {
    res = grheap_image_reader_string(r, &path->names[i], NULL);
  }
  return res;
}

static GRCORE_Result put_optional_string(GRCORE_SnapshotWriter * w, const char * text) {
  GRCORE_Result r = grcore_snapshot_writer_u64(w, text ? 1u : 0u);
  return r != GRCORE_OK || !text ? r : grcore_snapshot_writer_string(w, text);
}

static GRCORE_Result get_optional_string(GRCORE_SnapshotReader * r, const char ** out) {
  uint64_t present;
  GRCORE_Result res = grcore_snapshot_reader_u64(r, &present);
  *out = NULL;
  if (res != GRCORE_OK || !present) {
    return res;
  }
  return grcore_snapshot_reader_string(r, out, NULL);
}

/** A member: its library's path, then its own name. Refused if the names do not lead back to it. */
static GRCORE_Result core_put_member(const GLTANG_Execution * exec, const GLTANG_LibraryMember * member, GRCORE_SnapshotWriter * w) {
  LibraryPath path;
  const char * name;
  if (!member_path(exec, member, &path, &name)) {
    return GRCORE_ERR_INVALID;
  }
  GRCORE_Result r = core_put_path(w, &path);
  return r != GRCORE_OK ? r : grcore_snapshot_writer_string(w, name);
}

/** Reads a member and finds it in the destination's layers; `kind` says what it must be. */
static GRCORE_Result core_get_member(const GLTANG_Execution * exec, GRCORE_SnapshotReader * r, const GLTANG_LibraryMember ** out) {
  LibraryPath path;
  const char * name;
  GRCORE_Result res = core_get_path(r, &path);
  if (res == GRCORE_OK) {
    res = grcore_snapshot_reader_string(r, &name, NULL);
  }
  if (res != GRCORE_OK) {
    return res;
  }
  const GLTANG_Library * library = walk_path(exec, &path);
  const GLTANG_LibraryMember * member = library ? gltang_library_find(library, name, strlen(name)) : NULL;
  if (!member) {
    return GRCORE_ERR_INVALID; // a name the destination does not have
  }
  *out = member;
  return GRCORE_OK;
}

// ---------------------------------------------------------------------------
// The host values in the heap: per-object hooks (runtime-heap's image)
// ---------------------------------------------------------------------------

static GRHEAP_Result image_put_member(const GLTANG_Execution * exec, const GLTANG_LibraryMember * member, GRHEAP_ImageWriter * w) {
  LibraryPath path;
  const char * name;
  if (!member_path(exec, member, &path, &name)) {
    return GRHEAP_ERR_INVALID;
  }
  GRHEAP_Result r = image_put_path(w, &path);
  return r != GRHEAP_OK ? r : grheap_image_writer_string(w, name);
}

static GRHEAP_Result image_get_member(const GLTANG_Execution * exec, GRHEAP_ImageReader * r, GLTANG_MemberKind kind, bool or_builtin, const GLTANG_LibraryMember ** out) {
  LibraryPath path;
  const char * name;
  GRHEAP_Result res = image_get_path(r, &path);
  if (res == GRHEAP_OK) {
    res = grheap_image_reader_string(r, &name, NULL);
  }
  if (res != GRHEAP_OK) {
    return res;
  }
  const GLTANG_Library * library = walk_path(exec, &path);
  const GLTANG_LibraryMember * member = library ? gltang_library_find(library, name, strlen(name)) : NULL;
  if (!member || (member->kind != kind && !(or_builtin && member->kind == GLTANG_MEMBER_BUILTIN))) {
    return GRHEAP_ERR_INVALID; // a name the destination does not have, or has as something else
  }
  *out = member;
  return GRHEAP_OK;
}

GRHEAP_Result gltang_vm_library_snapshot(GRCORE_Context * context, void * payload, GRHEAP_ImageWriter * w) {
  const GLTANG_Execution * exec = gltang_vm_execution_of(context);
  GLTANG_LibraryObject * object = payload;
  LibraryPath path;
  if (!exec || !object->library || !library_path(exec, object->library, &path)) {
    return GRHEAP_ERR_INVALID;
  }
  GRHEAP_Result r = image_put_path(w, &path);
  object->library = NULL;
  return r;
}

GRHEAP_Result gltang_vm_library_restore(GRCORE_Context * context, void * payload, GRHEAP_ImageReader * r, void * user) {
  (void)user;
  const GLTANG_Execution * exec = gltang_vm_execution_of(context);
  GLTANG_LibraryObject * object = payload;
  LibraryPath path;
  GRHEAP_Result res = image_get_path(r, &path);
  if (res != GRHEAP_OK) {
    return res;
  }
  const GLTANG_Library * library = exec ? walk_path(exec, &path) : NULL;
  if (!library) {
    return GRHEAP_ERR_INVALID;
  }
  object->library = library;
  return GRHEAP_OK;
}

GRHEAP_Result gltang_vm_native_snapshot(GRCORE_Context * context, void * payload, GRHEAP_ImageWriter * w) {
  const GLTANG_Execution * exec = gltang_vm_execution_of(context);
  GLTANG_NativeObject * object = payload;
  if (!exec) {
    return GRHEAP_ERR_INVALID;
  }
  uint64_t present = object->member ? 1u : 0u;
  GRHEAP_Result r = grheap_image_writer_write(w, &present, sizeof present);
  if (r == GRHEAP_OK && object->member) {
    r = image_put_member(exec, object->member, w);
  }
  object->member = NULL;
  return r;
}

GRHEAP_Result gltang_vm_native_restore(GRCORE_Context * context, void * payload, GRHEAP_ImageReader * r, void * user) {
  (void)user;
  const GLTANG_Execution * exec = gltang_vm_execution_of(context);
  GLTANG_NativeObject * object = payload;
  uint64_t present;
  GRHEAP_Result res = grheap_image_reader_read(r, &present, sizeof present);
  if (res != GRHEAP_OK || !present) {
    return res;
  }
  const GLTANG_LibraryMember * member = NULL;
  if (!exec) {
    return GRHEAP_ERR_INVALID;
  }
  res = image_get_member(exec, r, GLTANG_MEMBER_NATIVE, true, &member);
  if (res == GRHEAP_OK) {
    object->member = member;
  }
  return res;
}

GRHEAP_Result gltang_vm_template_snapshot(GRCORE_Context * context, void * payload, GRHEAP_ImageWriter * w) {
  const GLTANG_Execution * exec = gltang_vm_execution_of(context);
  GLTANG_TemplateObject * object = payload;
  if (!exec || !object->member) {
    return GRHEAP_ERR_INVALID;
  }
  GRHEAP_Result r = image_put_member(exec, object->member, w);
  // What identifies the program, as the table of templates that have run
  // records it: a member of that name that holds other code would run it.
  const GLTANG_Program * program = object->member->program;
  uint64_t facts[4] = {0, 0, 0, 0};
  if (program) {
    facts[0] = gltang_program_identity(program);
    facts[1] = program->function_count;
    facts[2] = program->constant_count;
    facts[3] = program->global_count;
  }
  for (size_t i = 0; r == GRHEAP_OK && i < 4; ++i) {
    r = grheap_image_writer_write(w, &facts[i], sizeof facts[i]);
  }
  object->member = NULL;
  return r;
}

GRHEAP_Result gltang_vm_template_restore(GRCORE_Context * context, void * payload, GRHEAP_ImageReader * r, void * user) {
  (void)user;
  const GLTANG_Execution * exec = gltang_vm_execution_of(context);
  GLTANG_TemplateObject * object = payload;
  const GLTANG_LibraryMember * member = NULL;
  if (!exec) {
    return GRHEAP_ERR_INVALID;
  }
  GRHEAP_Result res = image_get_member(exec, r, GLTANG_MEMBER_TEMPLATE, false, &member);
  uint64_t facts[4];
  for (size_t i = 0; res == GRHEAP_OK && i < 4; ++i) {
    res = grheap_image_reader_read(r, &facts[i], sizeof facts[i]);
  }
  if (res == GRHEAP_OK) {
    // The same program, not merely a template of the same name.
    const GLTANG_Program * program = member->program;
    if (!program || gltang_program_identity(program) != facts[0] || program->function_count != facts[1] ||
        program->constant_count != facts[2] || program->global_count != facts[3]) {
      return GRHEAP_ERR_INVALID;
    }
    object->member = member;
  }
  return res;
}

/** The types of the engine, by the names a heap image records them under. */
static const GRHEAP_Type * resolve_type(void * user, const char * name) {
  (void)user;
  const GRHEAP_Type * const types[] = {
    &gltang_type_integer, &gltang_type_float, &gltang_type_string, &gltang_type_array,
    &gltang_type_array_store, &gltang_type_map, &gltang_type_map_store, &gltang_type_error,
    &gltang_type_library, &gltang_type_native, &gltang_type_template, &gltang_type_rng,
  };
  for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
    if (!strcmp(types[i]->name, name)) {
      return types[i];
    }
  }
  return NULL;
}

// ---------------------------------------------------------------------------
// The execution's blob: taking
// ---------------------------------------------------------------------------

GRCORE_Result gltang_vm_exec_snapshot(GRCORE_Context * context, void * value, GRCORE_SnapshotWriter * w) {
  (void)context;
  const GLTANG_Execution * exec = value;
  // A snapshot is of an execution that is not inside a call or a template, not
  // running, and has not ended: `NEW`, or paused at a poll.
  if (exec->destroyed || exec->in_host || exec->unwinding || exec->halted ||
      (exec->state != GLTANG_EXECUTION_NEW && exec->state != GLTANG_EXECUTION_PAUSED) ||
      exec->act != &exec->main_act) {
    return GRCORE_ERR_INVALID;
  }
  const GLTANG_Program * main = exec->programs[0].program;
  GRCORE_Result r = grcore_snapshot_writer_u64(w, XBLOB_MAGIC);
  uint64_t head[] = {
    exec->state, gltang_program_identity(main), main->function_count, main->constant_count, main->global_count,
    exec->root_count, exec->temp_count, exec->current_function, exec->current_offset, exec->main_act.result_lost ? 1u : 0u,
    exec->program_count - 1u,
  };
  for (size_t i = 0; r == GRCORE_OK && i < sizeof(head) / sizeof(head[0]); ++i) {
    r = grcore_snapshot_writer_u64(w, head[i]);
  }
  // The templates that have run, each named like a host value: a library and
  // a member, and what identifies its program.
  for (size_t p = 1; r == GRCORE_OK && p < exec->program_count; ++p) {
    const GLTANG_Program * program = exec->programs[p].program;
    const GLTANG_LibraryMember * member = find_template(exec, program);
    if (!member) {
      return GRCORE_ERR_INVALID;
    }
    r = core_put_member(exec, member, w);
    uint64_t facts[] = {gltang_program_identity(program), program->function_count, program->constant_count, program->global_count};
    for (size_t i = 0; r == GRCORE_OK && i < 4; ++i) {
      r = grcore_snapshot_writer_u64(w, facts[i]);
    }
  }
  // The output so far.
  const GLTANG_OutBuf * out = &exec->main_act.out;
  uint64_t out_head[] = {out->length, out->committed, out->segment_count};
  for (size_t i = 0; r == GRCORE_OK && i < 3; ++i) {
    r = grcore_snapshot_writer_u64(w, out_head[i]);
  }
  if (r == GRCORE_OK) {
    r = grcore_snapshot_writer_write(w, out->bytes, out->length);
  }
  for (size_t i = 0; r == GRCORE_OK && i < out->segment_count; ++i) {
    r = grcore_snapshot_writer_u64(w, out->segments[i].offset);
    if (r == GRCORE_OK) {
      r = grcore_snapshot_writer_u64(w, (uint64_t)out->segments[i].type);
    }
  }
  // The error list.
  if (r == GRCORE_OK) {
    r = grcore_snapshot_writer_u64(w, exec->error_count);
  }
  if (r == GRCORE_OK) {
    r = grcore_snapshot_writer_u64(w, exec->errors_dropped);
  }
  for (size_t i = 0; r == GRCORE_OK && i < exec->error_count; ++i) {
    const GLTANG_ErrorRecord * record = &exec->errors[i];
    const GLTANG_ErrorEntry * e = &record->entry;
    uint64_t fields[] = {(uint64_t)e->kind, (uint64_t)e->how, e->function, e->offset, (uint64_t)(int64_t)e->line, e->chain_count};
    for (size_t k = 0; r == GRCORE_OK && k < 6; ++k) {
      r = grcore_snapshot_writer_u64(w, fields[k]);
    }
    if (r == GRCORE_OK) {
      r = put_optional_string(w, e->template_name);
    }
    if (r == GRCORE_OK) {
      r = put_optional_string(w, e->file);
    }
    for (size_t k = 0; r == GRCORE_OK && k < e->chain_count; ++k) {
      r = put_optional_string(w, record->chain[k].template_name);
      if (r == GRCORE_OK) {
        r = put_optional_string(w, record->chain[k].file);
      }
      if (r == GRCORE_OK) {
        r = grcore_snapshot_writer_u64(w, (uint64_t)(int64_t)record->chain[k].line);
      }
    }
  }
  return r;
}

// ---------------------------------------------------------------------------
// The execution's blob: reading it
// ---------------------------------------------------------------------------

typedef struct ParsedLink {
  const char * template_name;
  const char * file;
  int line;
} ParsedLink;

typedef struct ParsedError {
  uint64_t kind, how, function, offset;
  int line;
  size_t chain_count;
  const char * template_name;
  const char * file;
  ParsedLink * links;
} ParsedError;

typedef struct Parsed {
  uint64_t state, main_hash, function_count, constant_count, global_count;
  uint64_t root_count, temp_count, current_function, current_offset, result_lost;
  size_t program_count;                    // the templates that have run
  const GLTANG_LibraryMember ** programs;  // their members, in this destination
  size_t out_length, out_committed, segment_count;
  const unsigned char * out_bytes;
  GLTANG_OutputSegment * segments;
  uint64_t dropped;
  size_t error_count;
  ParsedError * errors;
} Parsed;

static void parsed_free(GLTANG_Execution * exec, Parsed * p) {
  for (size_t i = 0; i < p->error_count; ++i) {
    gcu_allocator_free(exec->allocator, p->errors[i].links);
  }
  gcu_allocator_free(exec->allocator, p->errors);
  gcu_allocator_free(exec->allocator, p->segments);
  gcu_allocator_free(exec->allocator, (void *)p->programs);
  memset(p, 0, sizeof(*p));
}

/** The result for an allocation `exec`'s allocator refused. */
static GRCORE_Result alloc_failure(const GLTANG_Execution * exec, uint64_t refusals_before) {
  return grcore_context_memory_refusals(exec->context) > refusals_before ? GRCORE_ERR_LIMIT : GRCORE_ERR_OOM;
}

static GRCORE_Result read_count(GRCORE_SnapshotReader * r, size_t unit, size_t * out) {
  uint64_t n;
  GRCORE_Result res = grcore_snapshot_reader_u64(r, &n);
  if (res != GRCORE_OK) {
    return res;
  }
  // A count the rest of the blob could not possibly hold is not one.
  if (n > grcore_snapshot_reader_remaining(r) / unit) {
    return GRCORE_ERR_CORRUPT;
  }
  *out = (size_t)n;
  return GRCORE_OK;
}

/**
 * Reads the whole blob, checks it against itself and resolves the names in it
 * against the destination. Allocates through the execution's allocator, and
 * frees it all on failure; the strings stay views into the snapshot.
 */
static GRCORE_Result parse(GLTANG_Execution * exec, GRCORE_SnapshotReader * r, Parsed * p) {
  memset(p, 0, sizeof(*p));
  uint64_t refusals = grcore_context_memory_refusals(exec->context);
  uint64_t magic;
  GRCORE_Result res = grcore_snapshot_reader_u64(r, &magic);
  if (res != GRCORE_OK) {
    return res;
  }
  if (magic != XBLOB_MAGIC) {
    return GRCORE_ERR_CORRUPT;
  }
  uint64_t * fields[] = {&p->state, &p->main_hash, &p->function_count, &p->constant_count, &p->global_count,
    &p->root_count, &p->temp_count, &p->current_function, &p->current_offset, &p->result_lost};
  for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
    res = grcore_snapshot_reader_u64(r, fields[i]);
    if (res != GRCORE_OK) {
      return res;
    }
  }
  if ((p->state != GLTANG_EXECUTION_NEW && p->state != GLTANG_EXECUTION_PAUSED) || p->result_lost > 1u ||
      p->temp_count > UINT32_MAX || p->root_count > UINT32_MAX) {
    return GRCORE_ERR_CORRUPT;
  }
  res = read_count(r, 32, &p->program_count); // a template's entry is at least 57 bytes
  if (res != GRCORE_OK) {
    return res;
  }
  if (p->program_count) {
    p->programs = gcu_allocator_calloc(exec->allocator, p->program_count, sizeof(*p->programs));
    if (!p->programs) {
      p->program_count = 0;
      return alloc_failure(exec, refusals);
    }
  }
  for (size_t i = 0; res == GRCORE_OK && i < p->program_count; ++i) {
    const GLTANG_LibraryMember * member = NULL;
    res = core_get_member(exec, r, &member);
    uint64_t facts[4];
    for (size_t k = 0; res == GRCORE_OK && k < 4; ++k) {
      res = grcore_snapshot_reader_u64(r, &facts[k]);
    }
    if (res == GRCORE_OK) {
      // The template must be the same program: a member of that name that holds
      // something else would run other code on these frames.
      if (member->kind != GLTANG_MEMBER_TEMPLATE || !member->program || gltang_program_identity(member->program) != facts[0] ||
          member->program->function_count != facts[1] || member->program->constant_count != facts[2] ||
          member->program->global_count != facts[3]) {
        res = GRCORE_ERR_INVALID;
      }
      p->programs[i] = member;
    }
  }
  uint64_t out_head[3] = {0, 0, 0};
  for (size_t i = 0; res == GRCORE_OK && i < 3; ++i) {
    res = grcore_snapshot_reader_u64(r, &out_head[i]);
  }
  if (res == GRCORE_OK) {
    if (out_head[0] > grcore_snapshot_reader_remaining(r) || out_head[1] > out_head[0] ||
        out_head[2] > (grcore_snapshot_reader_remaining(r) - out_head[0]) / 16u) {
      res = GRCORE_ERR_CORRUPT;
    }
  }
  if (res == GRCORE_OK) {
    p->out_length = (size_t)out_head[0];
    p->out_committed = (size_t)out_head[1];
    p->segment_count = (size_t)out_head[2];
    const void * bytes;
    res = grcore_snapshot_reader_view(r, p->out_length, &bytes);
    p->out_bytes = bytes;
  }
  if (res == GRCORE_OK && p->segment_count) {
    p->segments = gcu_allocator_calloc(exec->allocator, p->segment_count, sizeof(*p->segments));
    if (!p->segments) {
      p->segment_count = 0;
      res = alloc_failure(exec, refusals);
    }
  }
  for (size_t i = 0; res == GRCORE_OK && i < p->segment_count; ++i) {
    uint64_t offset, type;
    res = grcore_snapshot_reader_u64(r, &offset);
    if (res == GRCORE_OK) {
      res = grcore_snapshot_reader_u64(r, &type);
    }
    if (res == GRCORE_OK) {
      if (offset > p->out_length || type > (uint64_t)GLTANG_UNICODE_STRING_TYPE_JAVASCRIPT ||
          (i && offset < p->segments[i - 1u].offset)) {
        res = GRCORE_ERR_CORRUPT;
      }
      p->segments[i].offset = (size_t)offset;
      p->segments[i].type = (GLTANG_String_Type)type;
    }
  }
  if (res == GRCORE_OK) {
    res = read_count(r, 64, &p->error_count); // an error's entry is at least 73 bytes
  }
  if (res == GRCORE_OK) {
    res = grcore_snapshot_reader_u64(r, &p->dropped);
  }
  if (res == GRCORE_OK && p->error_count) {
    p->errors = gcu_allocator_calloc(exec->allocator, p->error_count, sizeof(*p->errors));
    if (!p->errors) {
      p->error_count = 0;
      res = alloc_failure(exec, refusals);
    }
  }
  size_t entered = 0;
  for (; res == GRCORE_OK && entered < p->error_count; ++entered) {
    ParsedError * e = &p->errors[entered];
    uint64_t f[6];
    for (size_t k = 0; res == GRCORE_OK && k < 6; ++k) {
      res = grcore_snapshot_reader_u64(r, &f[k]);
    }
    if (res == GRCORE_OK) {
      res = get_optional_string(r, &e->template_name);
    }
    if (res == GRCORE_OK) {
      res = get_optional_string(r, &e->file);
    }
    if (res != GRCORE_OK) {
      break;
    }
    if (f[0] >= (uint64_t)GLTANG_ERROR_KIND_COUNT || f[1] > (uint64_t)GLTANG_ERROR_HOW_CREATED ||
        f[5] > grcore_snapshot_reader_remaining(r) / 24u || !e->template_name) {
      res = GRCORE_ERR_CORRUPT;
      break;
    }
    e->kind = f[0];
    e->how = f[1];
    e->function = f[2];
    e->offset = f[3];
    e->line = (int)(int64_t)f[4];
    e->chain_count = (size_t)f[5];
    if (e->chain_count) {
      e->links = gcu_allocator_calloc(exec->allocator, e->chain_count, sizeof(*e->links));
      if (!e->links) {
        e->chain_count = 0;
        res = alloc_failure(exec, refusals);
        break;
      }
    }
    for (size_t k = 0; res == GRCORE_OK && k < e->chain_count; ++k) {
      res = get_optional_string(r, &e->links[k].template_name);
      if (res == GRCORE_OK) {
        res = get_optional_string(r, &e->links[k].file);
      }
      uint64_t line;
      if (res == GRCORE_OK) {
        res = grcore_snapshot_reader_u64(r, &line);
      }
      if (res == GRCORE_OK) {
        e->links[k].line = (int)(int64_t)line;
        if (!e->links[k].template_name) {
          res = GRCORE_ERR_CORRUPT;
        }
      }
    }
  }
  if (res != GRCORE_OK) {
    parsed_free(exec, p); // the records not reached are zero, and free as nothing
    return res;
  }
  return GRCORE_OK;
}

/** Whether this destination is the one the blob was taken for, and fresh. */
static GRCORE_Result check_destination(const GLTANG_Execution * exec, const Parsed * p) {
  if (exec->destroyed || exec->in_host || exec->state != GLTANG_EXECUTION_NEW || exec->act != &exec->main_act ||
      exec->program_count != 1 || exec->error_count != 0 || exec->temp_count != 0 || exec->main_act.out.length != 0 ||
      exec->main_act.out.segment_count != 0 || exec->main_act.result != 0 || exec->halted || exec->unwinding) {
    return GRCORE_ERR_INVALID; // not fresh
  }
  const GLTANG_Program * main = exec->programs[0].program;
  if (gltang_program_identity(main) != p->main_hash || main->function_count != p->function_count ||
      main->constant_count != p->constant_count || main->global_count != p->global_count ||
      exec->root_count != p->root_count) {
    return GRCORE_ERR_INVALID; // another program
  }
  return GRCORE_OK;
}

GRCORE_Result gltang_vm_exec_restore(GRCORE_Context * context, void * value, GRCORE_SnapshotReader * reader, void * env, GRCORE_RestoreMode mode) {
  (void)context;
  (void)env;
  GLTANG_Execution * exec = value;
  Parsed p;
  GRCORE_Result r = parse(exec, reader, &p);
  if (r != GRCORE_OK) {
    return r; // parse has freed what it made
  }
  r = check_destination(exec, &p);
  if (r != GRCORE_OK || mode == GRCORE_RESTORE_CHECK) {
    parsed_free(exec, &p);
    return r;
  }

  // Everything that can fail comes first, into locals; what is installed after
  // is assignment.
  uint64_t refusals = grcore_context_memory_refusals(exec->context);
  const GCU_Allocator * a = exec->allocator;
  r = GRCORE_OK;
  unsigned char * out_bytes = NULL;
  GLTANG_Value * temps = NULL;
  GLTANG_ProgramEntry * programs = NULL;
  GLTANG_Value ** caches = NULL;
  GLTANG_ErrorRecord * errors = NULL;
  size_t made_caches = 0, made_errors = 0;
  if (p.out_length) {
    out_bytes = gcu_allocator_malloc(a, p.out_length + 1u);
    if (!out_bytes) {
      r = alloc_failure(exec, refusals);
    }
  }
  if (r == GRCORE_OK && p.temp_count) {
    temps = gcu_allocator_calloc(a, (size_t)p.temp_count, sizeof(GLTANG_Value));
    if (!temps) {
      r = alloc_failure(exec, refusals);
    }
  }
  if (r == GRCORE_OK && p.program_count) {
    programs = gcu_allocator_calloc(a, 1u + p.program_count, sizeof(GLTANG_ProgramEntry));
    caches = gcu_allocator_calloc(a, p.program_count, sizeof(*caches));
    if (!programs || !caches) {
      r = alloc_failure(exec, refusals);
    }
  }
  for (; r == GRCORE_OK && made_caches < p.program_count; ++made_caches) {
    size_t constants = p.programs[made_caches]->program->constant_count;
    caches[made_caches] = gcu_allocator_calloc(a, constants ? constants : 1u, sizeof(GLTANG_Value));
    if (!caches[made_caches]) {
      r = alloc_failure(exec, refusals);
      break;
    }
  }
  if (r == GRCORE_OK && p.error_count) {
    errors = gcu_allocator_calloc(a, p.error_count, sizeof(*errors));
    if (!errors) {
      r = alloc_failure(exec, refusals);
    }
  }
  for (; r == GRCORE_OK && made_errors < p.error_count; ++made_errors) {
    const ParsedError * e = &p.errors[made_errors];
    GLTANG_ErrorRecord * record = &errors[made_errors];
    size_t bytes = strlen(e->template_name) + 1u + (e->file ? strlen(e->file) + 1u : 0);
    for (size_t k = 0; k < e->chain_count; ++k) {
      bytes += strlen(e->links[k].template_name) + 1u + (e->links[k].file ? strlen(e->links[k].file) + 1u : 0);
    }
    record->text = gcu_allocator_malloc(a, bytes);
    if (e->chain_count) {
      record->chain = gcu_allocator_calloc(a, e->chain_count, sizeof(GLTANG_ErrorLink));
    }
    if (!record->text || (e->chain_count && !record->chain)) {
      r = alloc_failure(exec, refusals);
      ++made_errors; // this record owns what it did get
      break;
    }
    char * at = record->text;
    size_t length = strlen(e->template_name) + 1u;
    memcpy(at, e->template_name, length);
    record->entry.template_name = at;
    at += length;
    if (e->file) {
      length = strlen(e->file) + 1u;
      memcpy(at, e->file, length);
      record->entry.file = at;
      at += length;
    }
    for (size_t k = 0; k < e->chain_count; ++k) {
      length = strlen(e->links[k].template_name) + 1u;
      memcpy(at, e->links[k].template_name, length);
      record->chain[k].template_name = at;
      at += length;
      if (e->links[k].file) {
        length = strlen(e->links[k].file) + 1u;
        memcpy(at, e->links[k].file, length);
        record->chain[k].file = at;
        at += length;
      }
      record->chain[k].line = e->links[k].line;
    }
    record->entry.kind = (GLTANG_ErrorKind)e->kind;
    record->entry.message = gltang_error_kind_message(record->entry.kind);
    record->entry.how = (GLTANG_ErrorHow)e->how;
    record->entry.function = e->function;
    record->entry.offset = e->offset;
    record->entry.line = e->line;
    record->entry.chain_count = e->chain_count;
  }
  if (r != GRCORE_OK) {
    for (size_t i = 0; i < made_errors; ++i) {
      gcu_allocator_free(a, errors[i].text);
      gcu_allocator_free(a, errors[i].chain);
    }
    gcu_allocator_free(a, errors);
    for (size_t i = 0; i < made_caches; ++i) {
      gcu_allocator_free(a, caches[i]);
    }
    gcu_allocator_free(a, caches);
    gcu_allocator_free(a, programs);
    gcu_allocator_free(a, temps);
    gcu_allocator_free(a, out_bytes);
    parsed_free(exec, &p);
    return r;
  }

  // Installed: nothing below can fail.
  if (p.program_count) {
    programs[0] = exec->programs[0];
    for (size_t i = 0; i < p.program_count; ++i) {
      programs[1u + i].program = gltang_program_retain(p.programs[i]->program);
      programs[1u + i].constants = caches[i];
    }
    gcu_allocator_free(a, exec->programs);
    exec->programs = programs;
    exec->program_capacity = 1u + p.program_count;
    exec->program_count = 1u + p.program_count;
    gcu_allocator_free(a, caches);
  }
  if (p.temp_count) {
    gcu_allocator_free(a, exec->temps);
    exec->temps = temps;
    exec->temp_capacity = (size_t)p.temp_count;
    exec->temp_count = (size_t)p.temp_count;
  }
  GLTANG_OutBuf * out = &exec->main_act.out;
  if (p.out_length) {
    memcpy(out_bytes, p.out_bytes, p.out_length);
    out_bytes[p.out_length] = '\0';
    out->bytes = (char *)out_bytes;
    out->length = p.out_length;
    out->capacity = p.out_length + 1u;
    out->committed = p.out_committed;
  }
  if (p.segment_count) {
    out->segments = p.segments;
    out->segment_count = out->segment_capacity = p.segment_count;
    p.segments = NULL; // the execution owns them now
    p.segment_count = 0;
  }
  exec->errors = errors;
  exec->error_count = exec->error_capacity = p.error_count;
  exec->errors_dropped = p.dropped;
  exec->main_act.result_lost = p.result_lost != 0;
  exec->current_function = (uint32_t)p.current_function;
  exec->current_offset = (uint32_t)p.current_offset;
  exec->state = (GLTANG_ExecutionState)p.state;
  // What is left of the parse is views and the scratch arrays: the links the
  // records copied, the program table, and (when no segment moved) the segments.
  parsed_free(exec, &p);
  return GRCORE_OK;
}

GRCORE_Result gltang_vm_exec_settle(GRCORE_Context * context, void * value, void * env, GRCORE_SettleMode mode) {
  (void)context;
  (void)env;
  GLTANG_Execution * exec = value;
  if (mode != GRCORE_SETTLE_ABANDON) {
    return GRCORE_OK; // the heap writes the slots this blob made; nothing more here
  }
  // Back to what `gltang_execution_create` made. The root slots were never
  // written (that is COMMIT), so the heap needs nothing from this.
  const GCU_Allocator * a = exec->allocator;
  GLTANG_OutBuf * out = &exec->main_act.out;
  gcu_allocator_free(a, out->bytes);
  gcu_allocator_free(a, out->segments);
  memset(out, 0, sizeof(*out));
  gltang_vm_errors_free(exec);
  exec->errors_dropped = 0;
  for (size_t p = 1; p < exec->program_count; ++p) {
    gcu_allocator_free(a, exec->programs[p].constants);
    gltang_program_release(exec->programs[p].program);
    memset(&exec->programs[p], 0, sizeof(exec->programs[p]));
  }
  exec->program_count = 1;
  if (exec->temp_count) {
    memset(exec->temps, 0, exec->temp_count * sizeof(GLTANG_Value));
  }
  exec->temp_count = 0;
  exec->main_act.result_lost = false;
  exec->current_function = 0;
  exec->current_offset = 0;
  exec->state = GLTANG_EXECUTION_NEW;
  return GRCORE_OK;
}

// ---------------------------------------------------------------------------
// The host's side
// ---------------------------------------------------------------------------

static GLTANG_Result from_core(GRCORE_Result r) {
  switch (r) {
    case GRCORE_OK: return GLTANG_OK;
    case GRCORE_ERR_LIMIT: return GLTANG_ERR_LIMIT;
    case GRCORE_ERR_OOM: return GLTANG_ERR_OOM;
    default: return GLTANG_ERR_INVALID;
  }
}

GLTANG_Result gltang_snapshot_take(GLTANG_Execution * execution, GLTANG_Snapshot ** out_snapshot) {
  if (!execution || !out_snapshot || execution->destroyed || execution->in_host) {
    return GLTANG_ERR_INVALID;
  }
  GRCORE_Snapshot * snapshot = NULL;
  GRCORE_Result r = grcore_context_snapshot(execution->context, NULL, &snapshot);
  if (r != GRCORE_OK) {
    return from_core(r);
  }
  *out_snapshot = snapshot;
  return GLTANG_OK;
}

/** What the restore asks the host for, by the name of the key asking. */
typedef struct RestoreEnv {
  GLTANG_Execution * execution;
  GRHEAP_RestoreEnv heap;
} RestoreEnv;

static void * restore_lookup(void * user, const char * key_name) {
  RestoreEnv * env = user;
  if (!strcmp(key_name, GRHEAP_SNAPSHOT_KEY_NAME)) {
    return &env->heap;
  }
  return NULL;
}

GLTANG_Result gltang_snapshot_restore(GLTANG_Execution * execution, const GLTANG_Snapshot * snapshot) {
  if (!execution || !snapshot || execution->destroyed || execution->in_host || execution->state != GLTANG_EXECUTION_NEW) {
    return GLTANG_ERR_INVALID;
  }
  RestoreEnv env = {execution, GRHEAP_RESTORE_ENV_INIT(resolve_type, execution)};
  GRCORE_RestoreEnv core = GRCORE_RESTORE_ENV_INIT(NULL, NULL, NULL, NULL, NULL);
  core.user = &env;
  core.lookup = restore_lookup;
  core.entry = gltang_execution_entry;
  core.entry_state = execution;
  core.pause_file = execution->program ? execution->program->file : NULL;
  return from_core(grcore_context_restore(execution->context, snapshot, &core));
}

GLTANG_Snapshot * gltang_snapshot_retain(GLTANG_Snapshot * snapshot) {
  return grcore_snapshot_retain(snapshot);
}

void gltang_snapshot_release(GLTANG_Snapshot * snapshot) {
  grcore_snapshot_release(snapshot);
}

size_t gltang_snapshot_size(const GLTANG_Snapshot * snapshot) {
  return grcore_snapshot_size(snapshot);
}
