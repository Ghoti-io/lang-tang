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
 * The program object: reference counting, lookup and disassembly.
 */

#include <ghoti.io/lang-tang/macros.h>

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <ghoti.io/cutil/memory.h>
#include <ghoti.io/lang-tang/bytecode.h>
#include <ghoti.io/lang-tang/program.h>
#include "program_internal.h"
#include "../library/library_internal.h"
#include "../vm/string_layout.h"

GLTANG_Program * gltang_program_retain(GLTANG_Program * program) {
  if (program) {
    atomic_fetch_add_explicit(&program->references, 1, memory_order_relaxed);
  }
  return program;
}

void gltang_program_free(GLTANG_Program * program) {
  if (!program) {
    return;
  }
  for (uint32_t f = 0; f < program->function_count; ++f) {
    GLTANG_Function * function = &program->functions[f];
    gcu_free(function->name);
    gcu_free(function->code);
    gcu_free(function->lines);
    if (function->local_names) {
      for (uint32_t i = 0; i < function->local_count; ++i) {
        gcu_free(function->local_names[i]);
      }
      gcu_free(function->local_names);
    }
  }
  gcu_free(program->functions);
  for (uint32_t i = 0; i < program->constant_count; ++i) {
    gcu_free(program->constants[i].block);
  }
  gcu_free(program->constants);
  if (program->global_names) {
    for (uint32_t i = 0; i < program->global_count; ++i) {
      gcu_free(program->global_names[i]);
    }
    gcu_free(program->global_names);
  }
  gltang_library_release(program->libraries);
  gcu_free(program->file);
  gcu_free(program);
}

// FNV-1a over a stream of fields, each preceded by its length where it varies,
// so that two programs cannot hash alike by shifting bytes between fields.
static uint64_t mix_bytes(uint64_t hash, const void * data, size_t size) {
  const unsigned char * bytes = data;
  for (size_t i = 0; i < size; ++i) {
    hash = (hash ^ bytes[i]) * UINT64_C(0x100000001b3);
  }
  return hash;
}

static uint64_t mix_u64(uint64_t hash, uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    hash = (hash ^ ((value >> (8 * i)) & 0xFFu)) * UINT64_C(0x100000001b3);
  }
  return hash;
}

static uint64_t mix_string(uint64_t hash, const char * text) {
  if (!text) {
    return mix_u64(hash, UINT64_MAX);
  }
  size_t length = strlen(text);
  return mix_bytes(mix_u64(hash, length), text, length);
}

uint64_t gltang_program_identity(const GLTANG_Program * program) {
  uint64_t hash = UINT64_C(0xcbf29ce484222325);
  hash = mix_string(hash, program->file);
  hash = mix_u64(hash, program->function_count);
  for (uint32_t f = 0; f < program->function_count; ++f) {
    const GLTANG_Function * fn = &program->functions[f];
    hash = mix_string(hash, fn->name);
    hash = mix_u64(hash, fn->parameter_count);
    hash = mix_u64(hash, fn->local_count);
    hash = mix_u64(hash, fn->max_stack);
    hash = mix_u64(hash, fn->frame_slots);
    hash = mix_u64(hash, fn->code_count);
    hash = mix_bytes(hash, fn->code, (size_t)fn->code_count * sizeof(uint32_t));
    hash = mix_u64(hash, fn->line_count);
    for (uint32_t i = 0; i < fn->line_count; ++i) {
      hash = mix_u64(hash, ((uint64_t)fn->lines[i].pc << 32) | fn->lines[i].line);
    }
    for (uint32_t i = 0; fn->local_names && i < fn->local_count; ++i) {
      hash = mix_string(hash, fn->local_names[i]);
    }
  }
  hash = mix_u64(hash, program->constant_count);
  for (uint32_t i = 0; i < program->constant_count; ++i) {
    const GLTANG_Const * c = &program->constants[i];
    hash = mix_u64(hash, (uint64_t)c->kind);
    hash = mix_u64(hash, (uint64_t)c->integer);
    uint64_t bits;
    memcpy(&bits, &c->number, sizeof(bits));
    hash = mix_u64(hash, bits);
    hash = mix_u64(hash, c->block_size);
    hash = mix_bytes(hash, c->block, c->block_size);
  }
  hash = mix_u64(hash, program->global_count);
  for (uint32_t i = 0; program->global_names && i < program->global_count; ++i) {
    hash = mix_string(hash, program->global_names[i]);
  }
  return hash;
}

void gltang_program_release(GLTANG_Program * program) {
  if (!program) {
    return;
  }
  // The last reference frees. acq_rel so that every other thread's use of the
  // program happens before the free.
  if (atomic_fetch_sub_explicit(&program->references, 1, memory_order_acq_rel) == 1) {
    gltang_program_free(program);
  }
}

GLTANG_Result gltang_program_set_libraries(GLTANG_Program * program, GLTANG_Library * library) {
  // A program is shared by every context that runs it (AD-22), so it is
  // written only while one reference exists: the caller's.
  if (!program || atomic_load_explicit(&program->references, memory_order_acquire) != 1) {
    return GLTANG_ERR_INVALID;
  }
  GLTANG_Library * attached = gltang_library_attach(library);
  gltang_library_release(program->libraries);
  program->libraries = attached;
  return GLTANG_OK;
}

const char * gltang_program_file(const GLTANG_Program * program) {
  return program ? program->file : NULL;
}

size_t gltang_program_function_count(const GLTANG_Program * program) {
  return program ? program->function_count : 0;
}

const char * gltang_program_function_name(const GLTANG_Program * program, size_t function) {
  if (!program || function >= program->function_count) {
    return NULL;
  }
  return program->functions[function].name;
}

size_t gltang_program_function_size(const GLTANG_Program * program, size_t function) {
  if (!program || function >= program->function_count) {
    return 0;
  }
  return program->functions[function].code_count;
}

size_t gltang_program_function_max_stack(const GLTANG_Program * program, size_t function) {
  if (!program || function >= program->function_count) {
    return 0;
  }
  return program->functions[function].max_stack;
}

GLTANG_Result gltang_program_locate(const GLTANG_Program * program, uint64_t function, uint64_t offset, int * out_line) {
  if (!program || !out_line || function >= program->function_count) {
    return GLTANG_ERR_INVALID;
  }
  const GLTANG_Function * f = &program->functions[function];
  if (offset >= f->code_count || f->line_count == 0) {
    return GLTANG_ERR_INVALID;
  }
  // The last entry whose pc is at or before the offset.
  uint32_t low = 0;
  uint32_t high = f->line_count;
  while (low + 1 < high) {
    uint32_t middle = low + (high - low) / 2;
    if (f->lines[middle].pc <= offset) {
      low = middle;
    }
    else {
      high = middle;
    }
  }
  *out_line = (int)f->lines[low].line;
  return GLTANG_OK;
}

void gltang_program_dump(const GLTANG_Program * program, FILE * out) {
  if (!program || !out) {
    return;
  }
  fprintf(out, "program %s: %u functions, %u constants, %u globals\n", program->file, program->function_count, program->constant_count, program->global_count);
  for (uint32_t i = 0; i < program->constant_count; ++i) {
    const GLTANG_Const * c = &program->constants[i];
    switch (c->kind) {
      case GLTANG_CONST_INTEGER:
        fprintf(out, "  const %u: integer %lld\n", i, (long long)c->integer);
        break;
      case GLTANG_CONST_FLOAT:
        fprintf(out, "  const %u: float %g\n", i, c->number);
        break;
      case GLTANG_CONST_STRING: {
        const GLTANG_StringBlock * s = c->block;
        fprintf(out, "  const %u: string (%llu bytes, %llu graphemes) \"%.40s\"\n", i, (unsigned long long)s->byte_length, (unsigned long long)s->grapheme_length, gltang_string_bytes(s));
        break;
      }
    }
  }
  for (uint32_t f = 0; f < program->function_count; ++f) {
    const GLTANG_Function * function = &program->functions[f];
    fprintf(out, "function %u %s: %u parameters, %u locals, stack %u\n", f, function->name, function->parameter_count, function->local_count, function->max_stack);
    uint32_t line_index = 0;
    for (uint32_t pc = 0; pc < function->code_count; ++pc) {
      uint32_t word = function->code[pc];
      while (line_index + 1 < function->line_count && function->lines[line_index + 1].pc <= pc) {
        ++line_index;
      }
      fprintf(out, "  %4u  line %-4u %-12s %u", pc, function->line_count ? function->lines[line_index].line : 0, gltang_opcode_name(GLTANG_INSTRUCTION_OP(word)), GLTANG_INSTRUCTION_A(word));
      if (GLTANG_INSTRUCTION_OP(word) == GLTANG_OP_ITER_NEXT && pc + 1 < function->code_count) {
        ++pc;
        fprintf(out, " -> %u", function->code[pc]);
      }
      fputc('\n', out);
    }
  }
}
