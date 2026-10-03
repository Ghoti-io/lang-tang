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
 * The compiler: a syntax tree to a program.
 *
 * New code, written from the language reference and ctang's tests; nothing
 * here is ctang's compiler. One recursive walk over the tree, one function
 * builder per Tang function, statically resolved names (design.md, "How the
 * compiler resolves names"). The recursion is once per tree level and the
 * parser bounds the levels (GLTANG_MAX_TREE_DEPTH), so it is bounded.
 *
 * Failure policy: the first error stops the work (every step returns at once
 * when the compiler has failed), and everything allocated is freed whatever
 * the result.
 */

#include <ghoti.io/lang-tang/macros.h>

#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <ghoti.io/cutil/memory.h>
#include <ghoti.io/lang-tang/ast/astNodeAll.h>
#include <ghoti.io/lang-tang/bytecode.h>
#include <ghoti.io/lang-tang/compile.h>
#include "program_internal.h"
#include "../vm/string_layout.h"

// ---------------------------------------------------------------------------
// Small containers
// ---------------------------------------------------------------------------

typedef struct U32Vec {
  uint32_t * items;
  uint32_t count;
  uint32_t capacity;
} U32Vec;

static bool u32_push(U32Vec * vec, uint32_t value) {
  if (vec->count == vec->capacity) {
    uint32_t capacity = vec->capacity ? vec->capacity * 2u : 8u;
    if (capacity < vec->capacity) {
      return false;
    }
    uint32_t * items = gcu_realloc(vec->items, (size_t)capacity * sizeof(uint32_t));
    if (!items) {
      return false;
    }
    vec->items = items;
    vec->capacity = capacity;
  }
  vec->items[vec->count++] = value;
  return true;
}

static void u32_free(U32Vec * vec) {
  gcu_free(vec->items);
  vec->items = NULL;
  vec->count = vec->capacity = 0;
}

/** A scope's names: open addressing over an array that keeps insertion order. */
typedef struct Name {
  char * name;
  uint32_t slot;
  uint32_t hash;
  bool is_function;  ///< Declared by a function statement.
  bool global_ref;   ///< A function-local name bound to a program-scope slot.
} Name;

typedef struct NameTable {
  Name * items;
  uint32_t count;
  uint32_t capacity;
  uint32_t * buckets;  ///< Index + 1 into items; 0 is empty.
  uint32_t bucket_count;
} NameTable;

static uint32_t name_hash(const char * name) {
  uint32_t hash = 2166136261u;
  for (const unsigned char * c = (const unsigned char *)name; *c; ++c) {
    hash = (hash ^ *c) * 16777619u;
  }
  return hash;
}

static void name_table_free(NameTable * table) {
  for (uint32_t i = 0; i < table->count; ++i) {
    gcu_free(table->items[i].name);
  }
  gcu_free(table->items);
  gcu_free(table->buckets);
  memset(table, 0, sizeof(*table));
}

static Name * name_find(const NameTable * table, const char * name) {
  if (!table->bucket_count) {
    return NULL;
  }
  uint32_t hash = name_hash(name);
  uint32_t mask = table->bucket_count - 1u;
  for (uint32_t i = hash & mask;; i = (i + 1u) & mask) {
    uint32_t entry = table->buckets[i];
    if (!entry) {
      return NULL;
    }
    Name * item = &table->items[entry - 1u];
    if (item->hash == hash && !strcmp(item->name, name)) {
      return item;
    }
  }
}

static bool name_table_rehash(NameTable * table, uint32_t bucket_count) {
  uint32_t * buckets = gcu_calloc(bucket_count, sizeof(uint32_t));
  if (!buckets) {
    return false;
  }
  uint32_t mask = bucket_count - 1u;
  for (uint32_t n = 0; n < table->count; ++n) {
    uint32_t i = table->items[n].hash & mask;
    while (buckets[i]) {
      i = (i + 1u) & mask;
    }
    buckets[i] = n + 1u;
  }
  gcu_free(table->buckets);
  table->buckets = buckets;
  table->bucket_count = bucket_count;
  return true;
}

/** Adds a name that is not there. Returns NULL when memory runs out. */
static Name * name_add(NameTable * table, const char * name, uint32_t slot) {
  if (table->count == table->capacity) {
    uint32_t capacity = table->capacity ? table->capacity * 2u : 8u;
    Name * items = gcu_realloc(table->items, (size_t)capacity * sizeof(Name));
    if (!items) {
      return NULL;
    }
    table->items = items;
    table->capacity = capacity;
  }
  if ((table->count + 1u) * 2u > table->bucket_count) {
    if (!name_table_rehash(table, table->bucket_count ? table->bucket_count * 2u : 16u)) {
      return NULL;
    }
  }
  size_t length = strlen(name);
  char * copy = gcu_malloc(length + 1u);
  if (!copy) {
    return NULL;
  }
  memcpy(copy, name, length + 1u);
  Name * item = &table->items[table->count];
  *item = (Name){.name = copy, .slot = slot, .hash = name_hash(name)};
  ++table->count;
  uint32_t mask = table->bucket_count - 1u;
  uint32_t i = item->hash & mask;
  while (table->buckets[i]) {
    i = (i + 1u) & mask;
  }
  table->buckets[i] = table->count;
  return item;
}

// ---------------------------------------------------------------------------
// The compiler's state
// ---------------------------------------------------------------------------

typedef struct Loop {
  U32Vec breaks;     ///< Jumps to patch to the loop's exit.
  U32Vec continues;  ///< Jumps to patch to the loop's continue point.
} Loop;

typedef struct Fn {
  char * name;
  bool top;
  uint32_t * code;
  uint32_t code_count;
  uint32_t code_capacity;
  GLTANG_LineEntry * lines;
  uint32_t line_count;
  uint32_t line_capacity;
  uint32_t last_line;
  NameTable names;
  uint32_t local_count;
  char ** local_names;
  uint32_t local_name_capacity;
  uint32_t parameter_count;
  uint32_t max_stack;
  Loop * loops;
  uint32_t loop_count;
  uint32_t loop_capacity;
} Fn;

typedef struct Compiler {
  GLTANG_Result result;
  GLTANG_ParseError error;
  Fn ** fns;
  uint32_t fn_count;
  uint32_t fn_capacity;
  NameTable globals;
  GLTANG_Const * constants;
  uint32_t constant_count;
  uint32_t constant_capacity;
  const char * file;
} Compiler;

static void fail_oom(Compiler * c) {
  if (c->result == GLTANG_OK) {
    c->result = GLTANG_ERR_OOM;
  }
}

static void fail_limit(Compiler * c) {
  if (c->result == GLTANG_OK) {
    c->result = GLTANG_ERR_LIMIT;
  }
}

static void fail_format(Compiler * c, const GLTANG_Ast_Node * node, const char * format, const char * detail) {
  if (c->result != GLTANG_OK) {
    return;
  }
  c->result = GLTANG_ERR_FORMAT;
  c->error.line = node ? node->location.first_line + 1 : 1;
  c->error.column = node ? node->location.first_column + 1 : 1;
  snprintf(c->error.message, sizeof(c->error.message), format, detail ? detail : "");
}

// ---------------------------------------------------------------------------
// Function builders
// ---------------------------------------------------------------------------

static void fn_free(Fn * fn) {
  if (!fn) {
    return;
  }
  gcu_free(fn->name);
  gcu_free(fn->code);
  gcu_free(fn->lines);
  name_table_free(&fn->names);
  if (fn->local_names) {
    for (uint32_t i = 0; i < fn->local_count; ++i) {
      gcu_free(fn->local_names[i]);
    }
    gcu_free(fn->local_names);
  }
  for (uint32_t i = 0; i < fn->loop_count; ++i) {
    u32_free(&fn->loops[i].breaks);
    u32_free(&fn->loops[i].continues);
  }
  gcu_free(fn->loops);
  gcu_free(fn);
}

static Fn * fn_new(Compiler * c, const char * name, bool top) {
  if (c->result != GLTANG_OK) {
    return NULL;
  }
  Fn * fn = gcu_calloc(1, sizeof(Fn));
  if (!fn) {
    fail_oom(c);
    return NULL;
  }
  size_t length = strlen(name);
  fn->name = gcu_malloc(length + 1u);
  if (!fn->name) {
    gcu_free(fn);
    fail_oom(c);
    return NULL;
  }
  memcpy(fn->name, name, length + 1u);
  fn->top = top;
  if (c->fn_count == c->fn_capacity) {
    uint32_t capacity = c->fn_capacity ? c->fn_capacity * 2u : 8u;
    Fn ** fns = gcu_realloc(c->fns, (size_t)capacity * sizeof(Fn *));
    if (!fns) {
      fn_free(fn);
      fail_oom(c);
      return NULL;
    }
    c->fns = fns;
    c->fn_capacity = capacity;
  }
  c->fns[c->fn_count++] = fn;
  return fn;
}

static uint32_t fn_index(const Compiler * c, const Fn * fn) {
  for (uint32_t i = c->fn_count; i > 0; --i) {
    if (c->fns[i - 1u] == fn) {
      return i - 1u;
    }
  }
  return 0;
}

/** A new local slot; `name` is NULL for a hidden temporary. */
static uint32_t fn_new_local(Compiler * c, Fn * fn, const char * name) {
  if (c->result != GLTANG_OK) {
    return 0;
  }
  if (fn->local_count >= GLTANG_OPERAND_MAX - 2u) {
    fail_limit(c);
    return 0;
  }
  if (fn->local_count == fn->local_name_capacity) {
    uint32_t capacity = fn->local_name_capacity ? fn->local_name_capacity * 2u : 8u;
    char ** names = gcu_realloc(fn->local_names, (size_t)capacity * sizeof(char *));
    if (!names) {
      fail_oom(c);
      return 0;
    }
    fn->local_names = names;
    fn->local_name_capacity = capacity;
  }
  char * copy = NULL;
  if (name) {
    size_t length = strlen(name);
    copy = gcu_malloc(length + 1u);
    if (!copy) {
      fail_oom(c);
      return 0;
    }
    memcpy(copy, name, length + 1u);
  }
  fn->local_names[fn->local_count] = copy;
  return fn->local_count++;
}

static uint32_t emit(Compiler * c, Fn * fn, GLTANG_Opcode op, uint32_t a) {
  if (c->result != GLTANG_OK) {
    return 0;
  }
  if (a > GLTANG_OPERAND_MAX || fn->code_count >= GLTANG_OPERAND_MAX) {
    fail_limit(c);
    return 0;
  }
  if (fn->code_count == fn->code_capacity) {
    uint32_t capacity = fn->code_capacity ? fn->code_capacity * 2u : 32u;
    uint32_t * code = gcu_realloc(fn->code, (size_t)capacity * sizeof(uint32_t));
    if (!code) {
      fail_oom(c);
      return 0;
    }
    fn->code = code;
    fn->code_capacity = capacity;
  }
  fn->code[fn->code_count] = GLTANG_INSTRUCTION(op, a);
  return fn->code_count++;
}

/** The raw second word of a two-word instruction. */
static uint32_t emit_word(Compiler * c, Fn * fn, uint32_t word) {
  if (c->result != GLTANG_OK) {
    return 0;
  }
  if (fn->code_count >= GLTANG_OPERAND_MAX) {
    fail_limit(c);
    return 0;
  }
  if (fn->code_count == fn->code_capacity) {
    uint32_t capacity = fn->code_capacity ? fn->code_capacity * 2u : 32u;
    uint32_t * code = gcu_realloc(fn->code, (size_t)capacity * sizeof(uint32_t));
    if (!code) {
      fail_oom(c);
      return 0;
    }
    fn->code = code;
    fn->code_capacity = capacity;
  }
  fn->code[fn->code_count] = word;
  return fn->code_count++;
}

static void patch(Compiler * c, Fn * fn, uint32_t at, uint32_t target) {
  if (c->result != GLTANG_OK) {
    return;
  }
  if (target > GLTANG_OPERAND_MAX) {
    fail_limit(c);
    return;
  }
  fn->code[at] = GLTANG_INSTRUCTION(GLTANG_INSTRUCTION_OP(fn->code[at]), target);
}

static void mark_line(Compiler * c, Fn * fn, const GLTANG_Ast_Node * node) {
  if (c->result != GLTANG_OK || !node) {
    return;
  }
  uint32_t line = (uint32_t)(node->location.first_line + 1);
  if (line == fn->last_line) {
    return;
  }
  fn->last_line = line;
  if (fn->line_count && fn->lines[fn->line_count - 1u].pc == fn->code_count) {
    fn->lines[fn->line_count - 1u].line = line;
    return;
  }
  if (fn->line_count == fn->line_capacity) {
    uint32_t capacity = fn->line_capacity ? fn->line_capacity * 2u : 16u;
    GLTANG_LineEntry * lines = gcu_realloc(fn->lines, (size_t)capacity * sizeof(GLTANG_LineEntry));
    if (!lines) {
      fail_oom(c);
      return;
    }
    fn->lines = lines;
    fn->line_capacity = capacity;
  }
  fn->lines[fn->line_count++] = (GLTANG_LineEntry){.pc = fn->code_count, .line = line};
}

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static uint32_t const_add(Compiler * c, GLTANG_Const constant) {
  if (c->result != GLTANG_OK) {
    gcu_free(constant.block);
    return 0;
  }
  if (c->constant_count >= GLTANG_OPERAND_MAX) {
    gcu_free(constant.block);
    fail_limit(c);
    return 0;
  }
  if (c->constant_count == c->constant_capacity) {
    uint32_t capacity = c->constant_capacity ? c->constant_capacity * 2u : 16u;
    GLTANG_Const * constants = gcu_realloc(c->constants, (size_t)capacity * sizeof(GLTANG_Const));
    if (!constants) {
      gcu_free(constant.block);
      fail_oom(c);
      return 0;
    }
    c->constants = constants;
    c->constant_capacity = capacity;
  }
  c->constants[c->constant_count] = constant;
  return c->constant_count++;
}

static uint32_t const_integer(Compiler * c, int64_t value) {
  return const_add(c, (GLTANG_Const){.kind = GLTANG_CONST_INTEGER, .integer = value});
}

static uint32_t const_float(Compiler * c, double value) {
  return const_add(c, (GLTANG_Const){.kind = GLTANG_CONST_FLOAT, .number = value});
}

/** A constant string made of plain ASCII with no carriage return: one byte per grapheme. */
static uint32_t const_ascii(Compiler * c, const char * text, size_t length) {
  size_t size = gltang_string_block_size(1, length, length);
  void * block = size ? gcu_calloc(1, size) : NULL;
  if (!block) {
    fail_oom(c);
    return 0;
  }
  GLTANG_StringBlock * s = block;
  s->segment_count = 1;
  s->byte_length = length;
  s->grapheme_length = length;
  gltang_string_segments(s)[0] = GLTANG_SEGMENT_WORD(GLTANG_UNICODE_STRING_TYPE_TRUSTED, 0);
  memcpy(gltang_string_bytes(s), text, length);
  return const_add(c, (GLTANG_Const){.kind = GLTANG_CONST_STRING, .block = block, .block_size = size});
}

/** A constant string from a literal's string, keeping its segments and graphemes. */
static uint32_t const_string(Compiler * c, const GLTANG_Unicode_String * source) {
  size_t segments = source->string_type->count;
  size_t size = gltang_string_block_size(segments, source->byte_length, source->grapheme_length);
  void * block = size ? gcu_calloc(1, size) : NULL;
  if (!block) {
    fail_oom(c);
    return 0;
  }
  GLTANG_StringBlock * s = block;
  s->segment_count = (uint32_t)segments;
  s->byte_length = source->byte_length;
  s->grapheme_length = source->grapheme_length;
  for (size_t i = 0; i < segments; ++i) {
    gltang_string_segments(s)[i] = source->string_type->data[i].ui64;
  }
  uint32_t * offsets = gltang_string_offsets(s);
  if (offsets) {
    for (size_t i = 0; i <= source->grapheme_length; ++i) {
      offsets[i] = source->grapheme_offsets->data[i].ui32;
    }
  }
  memcpy(gltang_string_bytes(s), source->buffer, source->byte_length);
  return const_add(c, (GLTANG_Const){.kind = GLTANG_CONST_STRING, .block = block, .block_size = size});
}

// ---------------------------------------------------------------------------
// Name resolution
// ---------------------------------------------------------------------------

static const char * const MESSAGE_DECLARED_TWICE = "'%s' is already declared in this scope.";

typedef struct Binding {
  bool is_global;   ///< A program-scope slot, else a frame local.
  uint32_t slot;
} Binding;

/** Declares `name` in the program scope if it is not there. */
static Name * global_declare(Compiler * c, const char * name, bool is_function) {
  Name * existing = name_find(&c->globals, name);
  if (existing) {
    return existing;
  }
  if (c->globals.count >= GLTANG_OPERAND_MAX) {
    fail_limit(c);
    return NULL;
  }
  Name * added = name_add(&c->globals, name, c->globals.count);
  if (!added) {
    fail_oom(c);
    return NULL;
  }
  added->is_function = is_function;
  return added;
}

/**
 * Resolves `name` for a read, declaring it (as null until assigned) if it is
 * new: reading a name that was never assigned yields null, language reference
 * section 6.
 */
static bool resolve_read(Compiler * c, Fn * fn, const char * name, Binding * out) {
  if (c->result != GLTANG_OK) {
    return false;
  }
  if (fn->top) {
    Name * g = global_declare(c, name, false);
    if (!g) {
      return false;
    }
    *out = (Binding){true, g->slot};
    return true;
  }
  Name * local = name_find(&fn->names, name);
  if (local) {
    *out = (Binding){local->global_ref, local->slot};
    return true;
  }
  // A function declared in the program scope before this function was
  // declared is visible by name (ctang's rule), without `global`.
  Name * visible = name_find(&c->globals, name);
  if (visible && visible->is_function) {
    *out = (Binding){true, visible->slot};
    return true;
  }
  uint32_t slot = fn_new_local(c, fn, name);
  if (c->result != GLTANG_OK) {
    return false;
  }
  if (!name_add(&fn->names, name, slot)) {
    fail_oom(c);
    return false;
  }
  *out = (Binding){false, slot};
  return true;
}

/**
 * Resolves `name` as the target of an assignment, `use` or `global =`. A name
 * that is a function in scope cannot be assigned to: it was declared as one,
 * and declaring a name twice is an error (reference 13.18).
 */
static bool resolve_write(Compiler * c, Fn * fn, const GLTANG_Ast_Node * at, const char * name, Binding * out) {
  if (c->result != GLTANG_OK) {
    return false;
  }
  if (fn->top) {
    Name * g = global_declare(c, name, false);
    if (!g) {
      return false;
    }
    if (g->is_function) {
      fail_format(c, at, MESSAGE_DECLARED_TWICE, name);
      return false;
    }
    *out = (Binding){true, g->slot};
    return true;
  }
  Name * local = name_find(&fn->names, name);
  if (local) {
    if (local->is_function) {
      fail_format(c, at, MESSAGE_DECLARED_TWICE, name);
      return false;
    }
    if (local->global_ref) {
      Name * g = name_find(&c->globals, name);
      if (g && g->is_function) {
        fail_format(c, at, MESSAGE_DECLARED_TWICE, name);
        return false;
      }
    }
    *out = (Binding){local->global_ref, local->slot};
    return true;
  }
  Name * visible = name_find(&c->globals, name);
  if (visible && visible->is_function) {
    fail_format(c, at, MESSAGE_DECLARED_TWICE, name);
    return false;
  }
  uint32_t slot = fn_new_local(c, fn, name);
  if (c->result != GLTANG_OK) {
    return false;
  }
  if (!name_add(&fn->names, name, slot)) {
    fail_oom(c);
    return false;
  }
  *out = (Binding){false, slot};
  return true;
}

static void emit_load(Compiler * c, Fn * fn, Binding b) {
  emit(c, fn, b.is_global ? GLTANG_OP_LOAD_GLOBAL : GLTANG_OP_LOAD_LOCAL, b.slot);
}

static void emit_store(Compiler * c, Fn * fn, Binding b) {
  emit(c, fn, b.is_global ? GLTANG_OP_STORE_GLOBAL : GLTANG_OP_STORE_LOCAL, b.slot);
}

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

static void compile_expression(Compiler * c, Fn * fn, GLTANG_Ast_Node * node);
static void compile_statement(Compiler * c, Fn * fn, GLTANG_Ast_Node * node);

/** Whether an expression's value is a container nobody else holds. */
static bool is_fresh(const GLTANG_Ast_Node * node) {
  if (!node) {
    return true;
  }
  if (GLTANG_AST_IS_ARRAY(node) || GLTANG_AST_IS_MAP(node) || GLTANG_AST_IS_INTEGER(node)
      || GLTANG_AST_IS_FLOAT(node) || GLTANG_AST_IS_BOOLEAN(node) || GLTANG_AST_IS_STRING(node)
      || GLTANG_AST_IS_CAST(node) || GLTANG_AST_IS_SLICE(node) || GLTANG_AST_IS_PRINT(node)
      || GLTANG_AST_IS_UNARY(node)) {
    return true;
  }
  if (GLTANG_AST_IS_BINARY(node)) {
    GLTANG_Binary_Type t = ((const GLTANG_Ast_Node_Binary *)node)->operator_type;
    return t != GLTANG_BINARY_TYPE_AND && t != GLTANG_BINARY_TYPE_OR;
  }
  return node->vtable == &gltang_ast_node_null_vtable;
}

/** Stores into a container copy what is not fresh (reference section 3). */
static void compile_stored_value(Compiler * c, Fn * fn, GLTANG_Ast_Node * node) {
  compile_expression(c, fn, node);
  if (!is_fresh(node)) {
    emit(c, fn, GLTANG_OP_ADOPT, 0);
  }
}

/** Whether an optional part of the syntax is missing: the parser leaves either NULL or its null node. */
static bool is_absent(const GLTANG_Ast_Node * node) {
  return !node || node->vtable == &gltang_ast_node_null_vtable;
}

static uint32_t jump_here(const Fn * fn) {
  return fn->code_count;
}

static void compile_binary(Compiler * c, Fn * fn, GLTANG_Ast_Node_Binary * node) {
  GLTANG_Binary_Type type = node->operator_type;
  if (type == GLTANG_BINARY_TYPE_AND || type == GLTANG_BINARY_TYPE_OR) {
    compile_expression(c, fn, node->lhs);
    uint32_t jump = emit(c, fn, type == GLTANG_BINARY_TYPE_AND ? GLTANG_OP_AND : GLTANG_OP_OR, 0);
    compile_expression(c, fn, node->rhs);
    patch(c, fn, jump, jump_here(fn));
    return;
  }
  compile_expression(c, fn, node->lhs);
  compile_expression(c, fn, node->rhs);
  GLTANG_Opcode op = GLTANG_OP_ADD;
  switch (type) {
    case GLTANG_BINARY_TYPE_ADD: op = GLTANG_OP_ADD; break;
    case GLTANG_BINARY_TYPE_SUBTRACT: op = GLTANG_OP_SUB; break;
    case GLTANG_BINARY_TYPE_MULTIPLY: op = GLTANG_OP_MUL; break;
    case GLTANG_BINARY_TYPE_DIVIDE: op = GLTANG_OP_DIV; break;
    case GLTANG_BINARY_TYPE_MODULO: op = GLTANG_OP_MOD; break;
    case GLTANG_BINARY_TYPE_LESS_THAN: op = GLTANG_OP_LT; break;
    case GLTANG_BINARY_TYPE_LESS_THAN_EQUAL: op = GLTANG_OP_LE; break;
    case GLTANG_BINARY_TYPE_GREATER_THAN: op = GLTANG_OP_GT; break;
    case GLTANG_BINARY_TYPE_GREATER_THAN_EQUAL: op = GLTANG_OP_GE; break;
    case GLTANG_BINARY_TYPE_EQUAL: op = GLTANG_OP_EQ; break;
    case GLTANG_BINARY_TYPE_NOT_EQUAL: op = GLTANG_OP_NE; break;
    case GLTANG_BINARY_TYPE_AND:
    case GLTANG_BINARY_TYPE_OR:
      break;
  }
  emit(c, fn, op, 0);
}

static void compile_assign(Compiler * c, Fn * fn, GLTANG_Ast_Node_Assign * node) {
  GLTANG_Ast_Node * target = node->lhs;
  if (GLTANG_AST_IS_IDENTIFIER(target)) {
    Binding b;
    // The value is evaluated first: `a = a + 1` reads before it writes.
    compile_expression(c, fn, node->rhs);
    if (!resolve_write(c, fn, target, ((GLTANG_Ast_Node_Identifier *)target)->identifier, &b)) {
      return;
    }
    emit_store(c, fn, b);
  }
  else if (GLTANG_AST_IS_INDEX(target)) {
    GLTANG_Ast_Node_Index * index = (GLTANG_Ast_Node_Index *)target;
    compile_expression(c, fn, index->lhs);
    compile_expression(c, fn, index->rhs);
    compile_expression(c, fn, node->rhs);
    // The copy is taken by the store itself, after an array has grown to hold
    // it (reference 13.36), so storing an array into itself copies the
    // grown array.
    emit(c, fn, GLTANG_OP_SET_INDEX, is_fresh(node->rhs) ? 0u : 1u);
  }
  else if (GLTANG_AST_IS_PERIOD(target)) {
    GLTANG_Ast_Node_Period * period = (GLTANG_Ast_Node_Period *)target;
    compile_expression(c, fn, period->lhs);
    compile_expression(c, fn, node->rhs);
    uint32_t k = const_ascii(c, period->rhs, strlen(period->rhs));
    emit(c, fn, GLTANG_OP_SET_ATTR, k << 1 | (is_fresh(node->rhs) ? 0u : 1u));
  }
  else {
    fail_format(c, target, "%s", "Cannot assign to this expression.");
  }
}

static void compile_call(Compiler * c, Fn * fn, GLTANG_Ast_Node_Function_Call * node) {
  compile_expression(c, fn, node->lhs);
  uint32_t argc = (uint32_t)GLTANG_VECTORX_COUNT(node->arguments);
  for (uint32_t i = 0; i < argc; ++i) {
    compile_expression(c, fn, (GLTANG_Ast_Node *)GLTANG_TYPEX_P(node->arguments->data[i]));
  }
  emit(c, fn, GLTANG_OP_CALL, argc);
}

static void compile_slice(Compiler * c, Fn * fn, GLTANG_Ast_Node_Slice * node) {
  compile_expression(c, fn, node->lhs);
  uint32_t flags = 0;
  if (!is_absent(node->start)) {
    compile_expression(c, fn, node->start);
    flags |= 1u;
  }
  if (!is_absent(node->end)) {
    compile_expression(c, fn, node->end);
    flags |= 2u;
  }
  if (!is_absent(node->skip)) {
    compile_expression(c, fn, node->skip);
    flags |= 4u;
  }
  emit(c, fn, GLTANG_OP_SLICE, flags);
}

static void compile_expression(Compiler * c, Fn * fn, GLTANG_Ast_Node * node) {
  if (c->result != GLTANG_OK) {
    return;
  }
  mark_line(c, fn, node);
  if (!node || node->vtable == &gltang_ast_node_null_vtable) {
    emit(c, fn, GLTANG_OP_NULL, 0);
  }
  else if (GLTANG_AST_IS_IDENTIFIER(node)) {
    Binding b;
    if (resolve_read(c, fn, ((GLTANG_Ast_Node_Identifier *)node)->identifier, &b)) {
      emit_load(c, fn, b);
    }
  }
  else if (GLTANG_AST_IS_INTEGER(node)) {
    emit(c, fn, GLTANG_OP_CONST, const_integer(c, ((GLTANG_Ast_Node_Integer *)node)->value));
  }
  else if (GLTANG_AST_IS_BINARY(node)) {
    compile_binary(c, fn, (GLTANG_Ast_Node_Binary *)node);
  }
  else if (GLTANG_AST_IS_STRING(node)) {
    emit(c, fn, GLTANG_OP_CONST, const_string(c, ((GLTANG_Ast_Node_String *)node)->string));
  }
  else if (GLTANG_AST_IS_ASSIGN(node)) {
    compile_assign(c, fn, (GLTANG_Ast_Node_Assign *)node);
  }
  else if (GLTANG_AST_IS_FUNCTION_CALL(node)) {
    compile_call(c, fn, (GLTANG_Ast_Node_Function_Call *)node);
  }
  else if (GLTANG_AST_IS_FLOAT(node)) {
    emit(c, fn, GLTANG_OP_CONST, const_float(c, ((GLTANG_Ast_Node_Float *)node)->value));
  }
  else if (GLTANG_AST_IS_BOOLEAN(node)) {
    emit(c, fn, ((GLTANG_Ast_Node_Boolean *)node)->value ? GLTANG_OP_TRUE : GLTANG_OP_FALSE, 0);
  }
  else if (GLTANG_AST_IS_UNARY(node)) {
    GLTANG_Ast_Node_Unary * unary = (GLTANG_Ast_Node_Unary *)node;
    if (unary->operator_type == GLTANG_UNARY_TYPE_NEGATIVE && GLTANG_AST_IS_INTEGER(unary->expression)
        && ((GLTANG_Ast_Node_Integer *)unary->expression)->value != GLTANG_INTEGER_MIN) {
      emit(c, fn, GLTANG_OP_CONST, const_integer(c, -((GLTANG_Ast_Node_Integer *)unary->expression)->value));
    }
    else if (unary->operator_type == GLTANG_UNARY_TYPE_NEGATIVE && GLTANG_AST_IS_FLOAT(unary->expression)) {
      emit(c, fn, GLTANG_OP_CONST, const_float(c, -((GLTANG_Ast_Node_Float *)unary->expression)->value));
    }
    else {
      compile_expression(c, fn, unary->expression);
      emit(c, fn, unary->operator_type == GLTANG_UNARY_TYPE_NEGATIVE ? GLTANG_OP_NEG : GLTANG_OP_NOT, 0);
    }
  }
  else if (GLTANG_AST_IS_TERNARY(node)) {
    GLTANG_Ast_Node_Ternary * ternary = (GLTANG_Ast_Node_Ternary *)node;
    compile_expression(c, fn, ternary->condition);
    uint32_t to_else = emit(c, fn, GLTANG_OP_JMP_FALSE, 0);
    compile_expression(c, fn, ternary->ifTrue);
    uint32_t to_end = emit(c, fn, GLTANG_OP_JMP, 0);
    patch(c, fn, to_else, jump_here(fn));
    compile_expression(c, fn, ternary->ifFalse);
    patch(c, fn, to_end, jump_here(fn));
  }
  else if (GLTANG_AST_IS_CAST(node)) {
    GLTANG_Ast_Node_Cast * cast = (GLTANG_Ast_Node_Cast *)node;
    compile_expression(c, fn, cast->expression);
    emit(c, fn, GLTANG_OP_CAST, (uint32_t)cast->type);
  }
  else if (GLTANG_AST_IS_INDEX(node)) {
    GLTANG_Ast_Node_Index * index = (GLTANG_Ast_Node_Index *)node;
    compile_expression(c, fn, index->lhs);
    compile_expression(c, fn, index->rhs);
    emit(c, fn, GLTANG_OP_INDEX, 0);
  }
  else if (GLTANG_AST_IS_SLICE(node)) {
    compile_slice(c, fn, (GLTANG_Ast_Node_Slice *)node);
  }
  else if (GLTANG_AST_IS_PERIOD(node)) {
    GLTANG_Ast_Node_Period * period = (GLTANG_Ast_Node_Period *)node;
    compile_expression(c, fn, period->lhs);
    emit(c, fn, GLTANG_OP_ATTR, const_ascii(c, period->rhs, strlen(period->rhs)));
  }
  else if (GLTANG_AST_IS_PRINT(node)) {
    compile_expression(c, fn, ((GLTANG_Ast_Node_Print *)node)->expression);
    emit(c, fn, GLTANG_OP_PRINT, 0);
  }
  else if (GLTANG_AST_IS_ARRAY(node)) {
    GLTANG_Ast_Node_Array * array = (GLTANG_Ast_Node_Array *)node;
    uint32_t count = (uint32_t)GLTANG_VECTORX_COUNT(array->elements);
    for (uint32_t i = 0; i < count; ++i) {
      compile_stored_value(c, fn, (GLTANG_Ast_Node *)GLTANG_TYPEX_P(array->elements->data[i]));
    }
    emit(c, fn, GLTANG_OP_ARRAY, count);
  }
  else if (GLTANG_AST_IS_MAP(node)) {
    GLTANG_Ast_Node_Map * map = (GLTANG_Ast_Node_Map *)node;
    uint32_t count = (uint32_t)GLTANG_VECTORX_COUNT(map->pairs);
    for (uint32_t i = 0; i < count; ++i) {
      GLTANG_Ast_Node_Map_Pair * pair = (GLTANG_Ast_Node_Map_Pair *)GLTANG_TYPEX_P(map->pairs->data[i]);
      compile_expression(c, fn, pair->key);
      compile_stored_value(c, fn, pair->value);
    }
    emit(c, fn, GLTANG_OP_MAP, count);
  }
  else {
    // A statement node where an expression is required cannot come out of the
    // grammar, which has no such production.
    fail_format(c, node, "%s", "This is not an expression.");
  }
}

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

static Loop * loop_push(Compiler * c, Fn * fn) {
  if (c->result != GLTANG_OK) {
    return NULL;
  }
  if (fn->loop_count == fn->loop_capacity) {
    uint32_t capacity = fn->loop_capacity ? fn->loop_capacity * 2u : 4u;
    Loop * loops = gcu_realloc(fn->loops, (size_t)capacity * sizeof(Loop));
    if (!loops) {
      fail_oom(c);
      return NULL;
    }
    fn->loops = loops;
    fn->loop_capacity = capacity;
  }
  Loop * loop = &fn->loops[fn->loop_count++];
  memset(loop, 0, sizeof(*loop));
  return loop;
}

/** Closes the innermost loop: break jumps go to `exit`, continues to `again`. */
static void loop_pop(Compiler * c, Fn * fn, uint32_t exit, uint32_t again) {
  Loop * loop = &fn->loops[--fn->loop_count];
  for (uint32_t i = 0; i < loop->breaks.count; ++i) {
    patch(c, fn, loop->breaks.items[i], exit);
  }
  for (uint32_t i = 0; i < loop->continues.count; ++i) {
    patch(c, fn, loop->continues.items[i], again);
  }
  u32_free(&loop->breaks);
  u32_free(&loop->continues);
}

/** The program ends: its result is whatever is already set. */
static void emit_end_program(Compiler * c, Fn * fn) {
  emit(c, fn, GLTANG_OP_HALT, 0);
}

static void compile_function_declaration(Compiler * c, Fn * parent, GLTANG_Ast_Node_Function * node) {
  const char * name = node->identifier;
  Binding binding;
  if (parent->top) {
    Name * existing = name_find(&c->globals, name);
    if (existing) {
      fail_format(c, &node->base, MESSAGE_DECLARED_TWICE, name);
      return;
    }
    Name * g = global_declare(c, name, true);
    if (!g) {
      return;
    }
    binding = (Binding){true, g->slot};
  }
  else {
    if (name_find(&parent->names, name)) {
      fail_format(c, &node->base, MESSAGE_DECLARED_TWICE, name);
      return;
    }
    uint32_t slot = fn_new_local(c, parent, name);
    if (c->result != GLTANG_OK) {
      return;
    }
    Name * added = name_add(&parent->names, name, slot);
    if (!added) {
      fail_oom(c);
      return;
    }
    added->is_function = true;
    binding = (Binding){false, slot};
  }

  Fn * fn = fn_new(c, name, false);
  if (!fn) {
    return;
  }
  uint32_t index = fn_index(c, fn);
  // Parameters are the first locals, in order. A repeated name is an error
  // (reference 13.17): two parameters in one slot used to hang ctang.
  uint32_t parameter_count = (uint32_t)GLTANG_VECTORX_COUNT(node->parameters);
  for (uint32_t i = 0; i < parameter_count; ++i) {
    const char * parameter = ((GLTANG_Ast_Node_Identifier *)GLTANG_TYPEX_P(node->parameters->data[i]))->identifier;
    if (name_find(&fn->names, parameter)) {
      fail_format(c, &node->base, "Parameter '%s' is declared twice.", parameter);
      return;
    }
    uint32_t slot = fn_new_local(c, fn, parameter);
    if (c->result != GLTANG_OK) {
      return;
    }
    if (!name_add(&fn->names, parameter, slot)) {
      fail_oom(c);
      return;
    }
  }
  fn->parameter_count = parameter_count;

  // The poll at function entry (AD-4) is the first instruction.
  mark_line(c, fn, &node->base);
  emit(c, fn, GLTANG_OP_POLL, 0);
  compile_statement(c, fn, node->block);
  emit(c, fn, GLTANG_OP_NULL, 0);
  emit(c, fn, GLTANG_OP_RET, 0);

  mark_line(c, parent, &node->base);
  emit(c, parent, GLTANG_OP_FUNC, index);
  emit_store(c, parent, binding);
  emit(c, parent, GLTANG_OP_POP, 0);
}

/** Builds "a.b.c" from the library path of a `use`. */
static char * use_path(Compiler * c, GLTANG_Ast_Node * node) {
  size_t length = 0;
  for (GLTANG_Ast_Node * n = node;;) {
    if (GLTANG_AST_IS_LIBRARY(n)) {
      length += strlen(((GLTANG_Ast_Node_Library *)n)->identifier);
      break;
    }
    if (!GLTANG_AST_IS_PERIOD(n)) {
      fail_format(c, n, "%s", "This is not a library path.");
      return NULL;
    }
    length += strlen(((GLTANG_Ast_Node_Period *)n)->rhs) + 1u;
    n = ((GLTANG_Ast_Node_Period *)n)->lhs;
  }
  char * path = gcu_malloc(length + 1u);
  if (!path) {
    fail_oom(c);
    return NULL;
  }
  path[length] = '\0';
  size_t end = length;
  for (GLTANG_Ast_Node * n = node;;) {
    if (GLTANG_AST_IS_LIBRARY(n)) {
      const char * name = ((GLTANG_Ast_Node_Library *)n)->identifier;
      memcpy(path, name, strlen(name));
      break;
    }
    const char * member = ((GLTANG_Ast_Node_Period *)n)->rhs;
    size_t member_length = strlen(member);
    memcpy(path + end - member_length, member, member_length);
    path[end - member_length - 1u] = '.';
    end -= member_length + 1u;
    n = ((GLTANG_Ast_Node_Period *)n)->lhs;
  }
  return path;
}

static void compile_use(Compiler * c, Fn * fn, GLTANG_Ast_Node_Use * node) {
  char * path = use_path(c, node->expression);
  if (!path) {
    return;
  }
  uint32_t k = const_ascii(c, path, strlen(path));
  gcu_free(path);
  Binding b;
  if (!resolve_write(c, fn, &node->base, node->identifier, &b)) {
    return;
  }
  emit(c, fn, GLTANG_OP_USE, k);
  emit_store(c, fn, b);
  emit(c, fn, GLTANG_OP_POP, 0);
}

static void compile_global(Compiler * c, Fn * fn, GLTANG_Ast_Node_Global * node) {
  if (fn->top) {
    fail_format(c, &node->base, "%s", "A global declaration is only permitted inside a function.");
    return;
  }
  const char * name = ((GLTANG_Ast_Node_Identifier *)node->identifier)->identifier;
  if (name_find(&fn->names, name)) {
    fail_format(c, &node->base, MESSAGE_DECLARED_TWICE, name);
    return;
  }
  Name * g = global_declare(c, name, false);
  if (!g) {
    return;
  }
  Name * bound = name_add(&fn->names, name, g->slot);
  if (!bound) {
    fail_oom(c);
    return;
  }
  bound->global_ref = true;
  if (node->assignment) {
    compile_expression(c, fn, node->assignment);
    emit(c, fn, GLTANG_OP_POP, 0);
  }
}

static void compile_condition_jump(Compiler * c, Fn * fn, GLTANG_Ast_Node * condition, GLTANG_Opcode jump, uint32_t * out_jump) {
  compile_expression(c, fn, condition);
  *out_jump = emit(c, fn, jump, 0);
}

static void compile_loop_result(Compiler * c, Fn * fn) {
  // A loop's value is null (language reference, section 11).
  if (fn->top) {
    emit(c, fn, GLTANG_OP_CLEAR_RESULT, 0);
  }
}

static void compile_statement(Compiler * c, Fn * fn, GLTANG_Ast_Node * node) {
  if (c->result != GLTANG_OK) {
    return;
  }
  mark_line(c, fn, node);
  if (!node) {
    return;
  }
  if (GLTANG_AST_IS_BLOCK(node)) {
    GLTANG_Ast_Node_Block * block = (GLTANG_Ast_Node_Block *)node;
    uint32_t count = (uint32_t)GLTANG_VECTORX_COUNT(block->statements);
    if (!count && fn->top) {
      emit(c, fn, GLTANG_OP_CLEAR_RESULT, 0);
    }
    for (uint32_t i = 0; i < count; ++i) {
      compile_statement(c, fn, (GLTANG_Ast_Node *)GLTANG_TYPEX_P(block->statements->data[i]));
    }
  }
  else if (GLTANG_AST_IS_IF_ELSE(node)) {
    GLTANG_Ast_Node_If_Else * branch = (GLTANG_Ast_Node_If_Else *)node;
    uint32_t to_else;
    compile_condition_jump(c, fn, branch->condition, GLTANG_OP_JMP_FALSE, &to_else);
    compile_statement(c, fn, branch->ifBlock);
    if (branch->elseBlock || fn->top) {
      uint32_t to_end = emit(c, fn, GLTANG_OP_JMP, 0);
      patch(c, fn, to_else, jump_here(fn));
      if (branch->elseBlock) {
        compile_statement(c, fn, branch->elseBlock);
      }
      else {
        emit(c, fn, GLTANG_OP_CLEAR_RESULT, 0);
      }
      patch(c, fn, to_end, jump_here(fn));
    }
    else {
      patch(c, fn, to_else, jump_here(fn));
    }
  }
  else if (GLTANG_AST_IS_WHILE(node)) {
    GLTANG_Ast_Node_While * loop_node = (GLTANG_Ast_Node_While *)node;
    // The poll is at the head, which every iteration reaches, `continue`
    // included: a poll before the back jump would be skipped by it.
    uint32_t head = jump_here(fn);
    emit(c, fn, GLTANG_OP_POLL, 0);
    uint32_t to_exit;
    compile_condition_jump(c, fn, loop_node->condition, GLTANG_OP_JMP_FALSE, &to_exit);
    Loop * loop = loop_push(c, fn);
    (void)loop;
    compile_statement(c, fn, loop_node->block);
    emit(c, fn, GLTANG_OP_JMP, head);
    uint32_t exit = jump_here(fn);
    patch(c, fn, to_exit, exit);
    if (c->result == GLTANG_OK) {
      loop_pop(c, fn, exit, head);
    }
    compile_loop_result(c, fn);
  }
  else if (GLTANG_AST_IS_DO_WHILE(node)) {
    GLTANG_Ast_Node_Do_While * loop_node = (GLTANG_Ast_Node_Do_While *)node;
    uint32_t head = jump_here(fn);
    emit(c, fn, GLTANG_OP_POLL, 0);
    loop_push(c, fn);
    compile_statement(c, fn, loop_node->block);
    uint32_t again = jump_here(fn);
    compile_expression(c, fn, loop_node->condition);
    emit(c, fn, GLTANG_OP_JMP_TRUE, head);
    uint32_t exit = jump_here(fn);
    if (c->result == GLTANG_OK) {
      loop_pop(c, fn, exit, again);
    }
    compile_loop_result(c, fn);
  }
  else if (GLTANG_AST_IS_FOR(node)) {
    GLTANG_Ast_Node_For * loop_node = (GLTANG_Ast_Node_For *)node;
    if (!is_absent(loop_node->init)) {
      compile_expression(c, fn, loop_node->init);
      emit(c, fn, GLTANG_OP_POP, 0);
    }
    uint32_t head = jump_here(fn);
    emit(c, fn, GLTANG_OP_POLL, 0);
    uint32_t to_exit = 0;
    bool has_condition = !is_absent(loop_node->condition);
    if (has_condition) {
      compile_condition_jump(c, fn, loop_node->condition, GLTANG_OP_JMP_FALSE, &to_exit);
    }
    loop_push(c, fn);
    compile_statement(c, fn, loop_node->block);
    uint32_t again = jump_here(fn);
    if (!is_absent(loop_node->update)) {
      compile_expression(c, fn, loop_node->update);
      emit(c, fn, GLTANG_OP_POP, 0);
    }
    emit(c, fn, GLTANG_OP_JMP, head);
    uint32_t exit = jump_here(fn);
    if (has_condition) {
      patch(c, fn, to_exit, exit);
    }
    if (c->result == GLTANG_OK) {
      loop_pop(c, fn, exit, again);
    }
    compile_loop_result(c, fn);
  }
  else if (GLTANG_AST_IS_RANGED_FOR(node)) {
    GLTANG_Ast_Node_Ranged_For * loop_node = (GLTANG_Ast_Node_Ranged_For *)node;
    // Two hidden locals hold the array and the position, so that rebinding
    // the loop variable, or the name the array came from, cannot disturb the
    // iteration.
    uint32_t hidden = fn_new_local(c, fn, NULL);
    fn_new_local(c, fn, NULL);
    compile_expression(c, fn, loop_node->expression);
    emit(c, fn, GLTANG_OP_ITER_INIT, hidden);
    uint32_t to_skip = emit(c, fn, GLTANG_OP_JMP_FALSE, 0);
    uint32_t head = jump_here(fn);
    emit(c, fn, GLTANG_OP_POLL, 0);
    emit(c, fn, GLTANG_OP_ITER_NEXT, hidden);
    uint32_t to_exit = emit_word(c, fn, 0);
    Binding b;
    if (resolve_write(c, fn, loop_node->identifier, ((GLTANG_Ast_Node_Identifier *)loop_node->identifier)->identifier, &b)) {
      emit_store(c, fn, b);
      emit(c, fn, GLTANG_OP_POP, 0);
    }
    loop_push(c, fn);
    compile_statement(c, fn, loop_node->block);
    emit(c, fn, GLTANG_OP_JMP, head);
    uint32_t exit = jump_here(fn);
    if (c->result == GLTANG_OK) {
      fn->code[to_exit] = exit;
      loop_pop(c, fn, exit, head);
    }
    compile_loop_result(c, fn);
    patch(c, fn, to_skip, jump_here(fn));
  }
  else if (GLTANG_AST_IS_FUNCTION(node)) {
    compile_function_declaration(c, fn, (GLTANG_Ast_Node_Function *)node);
  }
  else if (GLTANG_AST_IS_RETURN(node)) {
    GLTANG_Ast_Node_Return * ret = (GLTANG_Ast_Node_Return *)node;
    compile_expression(c, fn, ret->expression);
    if (fn->top) {
      emit(c, fn, GLTANG_OP_SET_RESULT, 0);
      emit_end_program(c, fn);
    }
    else {
      emit(c, fn, GLTANG_OP_RET, 0);
    }
  }
  else if (GLTANG_AST_IS_BREAK(node) || GLTANG_AST_IS_CONTINUE(node)) {
    bool is_break = GLTANG_AST_IS_BREAK(node);
    if (fn->loop_count) {
      Loop * loop = &fn->loops[fn->loop_count - 1u];
      uint32_t jump = emit(c, fn, GLTANG_OP_JMP, 0);
      if (c->result == GLTANG_OK && !u32_push(is_break ? &loop->breaks : &loop->continues, jump)) {
        fail_oom(c);
      }
    }
    else if (fn->top) {
      // Outside a loop, at the top level, either one ends the program with
      // a null result (reference 5.7).
      emit(c, fn, GLTANG_OP_CLEAR_RESULT, 0);
      emit_end_program(c, fn);
    }
    else {
      // ...and inside a function, returns null from it.
      emit(c, fn, GLTANG_OP_NULL, 0);
      emit(c, fn, GLTANG_OP_RET, 0);
    }
  }
  else if (GLTANG_AST_IS_USE(node)) {
    compile_use(c, fn, (GLTANG_Ast_Node_Use *)node);
  }
  else if (GLTANG_AST_IS_GLOBAL(node)) {
    compile_global(c, fn, (GLTANG_Ast_Node_Global *)node);
  }
  else if (GLTANG_AST_IS_PRINT(node) && GLTANG_AST_IS_STRING(((GLTANG_Ast_Node_Print *)node)->expression)) {
    // Template text: print a constant without making it a value first.
    GLTANG_Ast_Node_String * text = (GLTANG_Ast_Node_String *)((GLTANG_Ast_Node_Print *)node)->expression;
    emit(c, fn, GLTANG_OP_PRINT_CONST, const_string(c, text->string));
    if (fn->top) {
      emit(c, fn, GLTANG_OP_CLEAR_RESULT, 0);
    }
  }
  else {
    // An expression statement. At the top level its value is the program's
    // result for as long as no later statement replaces it.
    compile_expression(c, fn, node);
    emit(c, fn, fn->top ? GLTANG_OP_SET_RESULT : GLTANG_OP_POP, 0);
  }
}

// ---------------------------------------------------------------------------
// The operand stack's depth
// ---------------------------------------------------------------------------

/** The pops and pushes of an instruction; `words` is 2 for a two-word one. */
static void stack_effect(GLTANG_Opcode op, uint32_t a, int32_t * pops, int32_t * pushes) {
  *pops = 0;
  *pushes = 0;
  switch (op) {
    case GLTANG_OP_POP: case GLTANG_OP_SET_RESULT: case GLTANG_OP_JMP_FALSE:
    case GLTANG_OP_JMP_TRUE: case GLTANG_OP_RET:
      *pops = 1; break;
    case GLTANG_OP_DUP: *pops = 1; *pushes = 2; break;
    case GLTANG_OP_NULL: case GLTANG_OP_TRUE: case GLTANG_OP_FALSE: case GLTANG_OP_CONST:
    case GLTANG_OP_LOAD_LOCAL: case GLTANG_OP_LOAD_GLOBAL: case GLTANG_OP_FUNC:
    case GLTANG_OP_USE:
      *pushes = 1; break;
    case GLTANG_OP_STORE_LOCAL: case GLTANG_OP_STORE_GLOBAL: case GLTANG_OP_NEG:
    case GLTANG_OP_NOT: case GLTANG_OP_CAST: case GLTANG_OP_ATTR: case GLTANG_OP_ADOPT:
    case GLTANG_OP_PRINT: case GLTANG_OP_ITER_INIT:
      *pops = 1; *pushes = 1; break;
    case GLTANG_OP_ADD: case GLTANG_OP_SUB: case GLTANG_OP_MUL: case GLTANG_OP_DIV:
    case GLTANG_OP_MOD: case GLTANG_OP_LT: case GLTANG_OP_LE: case GLTANG_OP_GT:
    case GLTANG_OP_GE: case GLTANG_OP_EQ: case GLTANG_OP_NE: case GLTANG_OP_INDEX:
    case GLTANG_OP_SET_ATTR:
      *pops = 2; *pushes = 1; break;
    case GLTANG_OP_SET_INDEX: *pops = 3; *pushes = 1; break;
    case GLTANG_OP_SLICE:
      *pops = 1 + (int32_t)((a & 1u) + ((a >> 1) & 1u) + ((a >> 2) & 1u)); *pushes = 1; break;
    case GLTANG_OP_ARRAY: *pops = (int32_t)a; *pushes = 1; break;
    case GLTANG_OP_MAP: *pops = (int32_t)(2u * a); *pushes = 1; break;
    case GLTANG_OP_CALL: *pops = (int32_t)a + 1; *pushes = 1; break;
    case GLTANG_OP_AND: case GLTANG_OP_OR: *pops = 1; break;  // on the fall-through path
    case GLTANG_OP_ITER_NEXT: *pushes = 1; break;              // on the fall-through path
    default: break;
  }
}

/**
 * Works out the deepest the operand stack gets, and checks that every path
 * reaches an instruction at one depth. Instructions nothing reaches (the code
 * after a `return`) are skipped.
 */
static bool compute_max_stack(Compiler * c, Fn * fn) {
  uint32_t n = fn->code_count;
  if (!n) {
    return true;
  }
  int32_t * depth = gcu_malloc((size_t)n * sizeof(int32_t));
  uint32_t * work = gcu_malloc((size_t)n * sizeof(uint32_t) + sizeof(uint32_t));
  if (!depth || !work) {
    gcu_free(depth);
    gcu_free(work);
    fail_oom(c);
    return false;
  }
  for (uint32_t i = 0; i < n; ++i) {
    depth[i] = -1;
  }
  uint32_t top = 0;
  int32_t max = 0;
  bool ok = true;
  depth[0] = 0;
  work[top++] = 0;
  // Each instruction is pushed at most once per depth it is first reached at,
  // and a second arrival must agree, so the work list is bounded by n.
  while (top && ok) {
    uint32_t pc = work[--top];
    int32_t d = depth[pc];
    for (;;) {
      uint32_t word = fn->code[pc];
      GLTANG_Opcode op = GLTANG_INSTRUCTION_OP(word);
      uint32_t a = GLTANG_INSTRUCTION_A(word);
      int32_t pops, pushes;
      stack_effect(op, a, &pops, &pushes);
      if (d < pops) {
        ok = false;
        break;
      }
      int32_t after = d - pops + pushes;
      if (d > max) {
        max = d;
      }
      if (after > max) {
        max = after;
      }
      uint32_t next = pc + 1u;
      // Branch targets.
      bool falls = true;
      uint32_t target = 0;
      int32_t target_depth = after;
      bool has_target = false;
      switch (op) {
        case GLTANG_OP_HALT: case GLTANG_OP_RET: falls = false; break;
        case GLTANG_OP_JMP: falls = false; has_target = true; target = a; break;
        case GLTANG_OP_JMP_FALSE: case GLTANG_OP_JMP_TRUE: has_target = true; target = a; break;
        case GLTANG_OP_AND: case GLTANG_OP_OR: has_target = true; target = a; target_depth = d; break;
        case GLTANG_OP_ITER_NEXT:
          if (next >= n) {
            ok = false;
          }
          else {
            has_target = true; target = fn->code[next]; target_depth = d; next = pc + 2u;
          }
          break;
        default: break;
      }
      if (!ok) {
        break;
      }
      if (has_target) {
        if (target >= n) {
          ok = false;
          break;
        }
        if (depth[target] < 0) {
          depth[target] = target_depth;
          work[top++] = target;
        }
        else if (depth[target] != target_depth) {
          ok = false;
          break;
        }
      }
      if (!falls) {
        break;
      }
      if (next >= n) {
        // Falling off the end of a function: the compiler always ends one
        // with an instruction that does not fall.
        ok = false;
        break;
      }
      if (depth[next] >= 0) {
        if (depth[next] != after) {
          ok = false;
        }
        break;
      }
      depth[next] = after;
      pc = next;
      d = after;
    }
  }
  gcu_free(depth);
  gcu_free(work);
  if (!ok) {
    if (c->result == GLTANG_OK) {
      c->result = GLTANG_ERR_INTERNAL;
    }
    return false;
  }
  fn->max_stack = (uint32_t)max;
  return true;
}

// ---------------------------------------------------------------------------
// Assembling the program
// ---------------------------------------------------------------------------

static char * duplicate(const char * s) {
  size_t length = strlen(s);
  char * copy = gcu_malloc(length + 1u);
  if (copy) {
    memcpy(copy, s, length + 1u);
  }
  return copy;
}

static GLTANG_Program * build_program(Compiler * c) {
  GLTANG_Program * program = gcu_calloc(1, sizeof(GLTANG_Program));
  if (!program) {
    fail_oom(c);
    return NULL;
  }
  atomic_init(&program->references, 1);
  program->file = duplicate(c->file);
  program->functions = gcu_calloc(c->fn_count ? c->fn_count : 1u, sizeof(GLTANG_Function));
  program->global_names = gcu_calloc(c->globals.count ? c->globals.count : 1u, sizeof(char *));
  if (!program->file || !program->functions || !program->global_names) {
    gltang_program_free(program);
    fail_oom(c);
    return NULL;
  }
  // From here on the program owns the parts it has been given, so a failure
  // frees through it.
  program->function_count = c->fn_count;
  program->global_count = c->globals.count;
  for (uint32_t i = 0; i < c->globals.count; ++i) {
    program->global_names[i] = duplicate(c->globals.items[i].name);
    if (!program->global_names[i]) {
      program->global_count = i;
      gltang_program_free(program);
      fail_oom(c);
      return NULL;
    }
  }
  for (uint32_t i = 0; i < c->fn_count; ++i) {
    Fn * fn = c->fns[i];
    GLTANG_Function * out = &program->functions[i];
    // Parts are moved, not copied: the builder forgets them.
    out->name = fn->name;
    fn->name = NULL;
    out->parameter_count = fn->parameter_count;
    out->local_count = fn->local_count;
    out->max_stack = fn->max_stack;
    out->frame_slots = GLTANG_FRAME_HEADER + fn->local_count + fn->max_stack;
    out->code = fn->code;
    out->code_count = fn->code_count;
    fn->code = NULL;
    out->lines = fn->lines;
    out->line_count = fn->line_count;
    fn->lines = NULL;
    out->local_names = fn->local_names;
    fn->local_names = NULL;
    if (!out->local_names && fn->local_count) {
      gltang_program_free(program);
      fail_oom(c);
      return NULL;
    }
    if (!out->local_names) {
      out->local_names = gcu_calloc(1, sizeof(char *));
      if (!out->local_names) {
        gltang_program_free(program);
        fail_oom(c);
        return NULL;
      }
    }
    if (!out->code) {
      out->code = gcu_calloc(1, sizeof(uint32_t));
      if (!out->code) {
        gltang_program_free(program);
        fail_oom(c);
        return NULL;
      }
    }
  }
  program->constants = c->constants;
  program->constant_count = c->constant_count;
  c->constants = NULL;
  c->constant_count = 0;
  return program;
}

GLTANG_Result gltang_compile(const GLTANG_Tree * tree, const char * file_name, GLTANG_ParseError * error_out, GLTANG_Program ** program_out) {
  if (!tree || !program_out) {
    return GLTANG_ERR_INVALID;
  }
  Compiler compiler;
  memset(&compiler, 0, sizeof(compiler));
  compiler.result = GLTANG_OK;
  compiler.file = file_name ? file_name : "<program>";

  Fn * top = fn_new(&compiler, "<program>", true);
  GLTANG_Program * program = NULL;
  if (top) {
    mark_line(&compiler, top, gltang_tree_root(tree));
    emit(&compiler, top, GLTANG_OP_POLL, 0);
    GLTANG_Ast_Node * root = gltang_tree_root(tree);
    if (root) {
      // The root is a block of statements, or a lone expression.
      compile_statement(&compiler, top, root);
    }
    emit_end_program(&compiler, top);
    for (uint32_t i = 0; i < compiler.fn_count && compiler.result == GLTANG_OK; ++i) {
      compute_max_stack(&compiler, compiler.fns[i]);
    }
    if (compiler.result == GLTANG_OK) {
      program = build_program(&compiler);
    }
  }

  GLTANG_Result result = compiler.result;
  if (result == GLTANG_ERR_FORMAT && error_out) {
    *error_out = compiler.error;
  }
  for (uint32_t i = 0; i < compiler.fn_count; ++i) {
    fn_free(compiler.fns[i]);
  }
  gcu_free(compiler.fns);
  name_table_free(&compiler.globals);
  for (uint32_t i = 0; i < compiler.constant_count; ++i) {
    gcu_free(compiler.constants[i].block);
  }
  gcu_free(compiler.constants);
  if (result == GLTANG_OK) {
    *program_out = program;
  }
  else if (program) {
    gltang_program_free(program);
  }
  return result;
}
