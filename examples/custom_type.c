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
 * A value type of the host's own: `money`, an amount in cents, that a
 * template adds, compares and reads like one of the language's values.
 *
 *  - the type is a descriptor (type.h) added to a registry once, for the whole
 *    process; an execution attaches the registry before it runs;
 *  - an instance holds its amount in raw bytes (plain data the collector never
 *    reads) and its currency in a value slot (a string the collector keeps);
 *  - `add` answers for money and money, and money and an integer, on either
 *    side: `3 + price` reaches money's add with `self_is_lhs` false. Anything
 *    else it declines, and a decline from both sides is `Not supported`;
 *  - `less` compares; `amount` and `currency` are attributes; with no `equal`,
 *    two moneys are equal only if they are one object.
 */

#include <ghoti.io/lang-tang/compile.h>
#include <ghoti.io/lang-tang/execution.h>
#include <ghoti.io/lang-tang/handle.h>
#include <ghoti.io/lang-tang/lang-tang.h>
#include <ghoti.io/lang-tang/library.h>
#include <ghoti.io/lang-tang/program.h>
#include <ghoti.io/lang-tang/type.h>
#include <ghoti.io/lang-tang/value.h>
#include <ghoti.io/runtime-core/runtime-core.h>
#include <ghoti.io/runtime-heap/runtime-heap.h>

#include <stdio.h>
#include <string.h>

static const GLTANG_Type * money_type;

static int64_t cents(GLTANG_NativeCall * call, GLTANG_Handle money) {
  int64_t amount = 0;
  (void)gltang_handle_custom_read_bytes(call, money, 0, &amount, sizeof amount);
  return amount;
}

/* A new money of `amount` cents, its currency copied from `like`. */
static GLTANG_OpStatus answer_money(GLTANG_NativeCall * call, int64_t amount, GLTANG_Handle like) {
  GLTANG_Handle result, currency;
  if (gltang_handle_make_custom(call, money_type, &result) != GLTANG_OK
      || gltang_handle_custom_write_bytes(call, result, 0, &amount, sizeof amount) != GLTANG_OK
      || gltang_handle_custom_slot_get(call, like, 0, &currency) != GLTANG_OK
      || gltang_handle_custom_slot_set(call, result, 0, currency) != GLTANG_OK) {
    return GLTANG_OP_ERROR;
  }
  gltang_call_return_handle(call, result);
  return GLTANG_OP_OK;
}

/* The other operand in cents: a money's, or an integer's. */
static int other_cents(GLTANG_NativeCall * call, GLTANG_Handle other, int64_t * out) {
  if (gltang_type_same(gltang_handle_custom_type(call, other), money_type)) {
    *out = cents(call, other);
    return 1;
  }
  if (gltang_handle_kind(call, other) == GLTANG_KIND_INTEGER) {
    *out = gltang_handle_integer(call, other);
    return 1;
  }
  return 0;
}

static GLTANG_OpStatus money_add(GLTANG_NativeCall * call, void * user, GLTANG_Handle self, GLTANG_Handle other, bool self_is_lhs, bool is_assignment) {
  (void)user;
  (void)self_is_lhs;   /* addition is the same either way round */
  (void)is_assignment;
  int64_t b;
  if (!other_cents(call, other, &b)) {
    return GLTANG_OP_DECLINE;   /* not a rule of ours: the other side, or Not supported */
  }
  return answer_money(call, cents(call, self) + b, self);
}

static GLTANG_OpStatus money_less(GLTANG_NativeCall * call, void * user, GLTANG_Handle self, GLTANG_Handle other, bool self_is_lhs) {
  (void)user;
  int64_t a = cents(call, self), b;
  if (!other_cents(call, other, &b)) {
    return GLTANG_OP_DECLINE;
  }
  /* The reflected call computes `other < self`. */
  gltang_call_return_bool(call, self_is_lhs ? a < b : b < a);
  return GLTANG_OP_OK;
}

static GLTANG_OpStatus money_attribute(GLTANG_NativeCall * call, void * user, GLTANG_Handle self, GLTANG_Handle name) {
  (void)user;
  size_t length = 0;
  const char * text = gltang_handle_text(call, name, &length);
  if (text && length == 6 && memcmp(text, "amount", 6) == 0) {
    gltang_call_return_integer(call, cents(call, self));
    return GLTANG_OP_OK;
  }
  GLTANG_Handle currency;
  if (text && length == 8 && memcmp(text, "currency", 8) == 0 && gltang_handle_custom_slot_get(call, self, 0, &currency) == GLTANG_OK) {
    gltang_call_return_handle(call, currency);
    return GLTANG_OP_OK;
  }
  return GLTANG_OP_DECLINE;   /* `Not implemented`, as for any value with no such attribute */
}

/* money(cents, currency): the constructor a program calls. */
static bool make_money(GLTANG_NativeCall * call, void * user) {
  (void)user;
  int64_t amount = gltang_call_integer(call, 0);
  GLTANG_Handle money, currency;
  if (gltang_handle_make_custom(call, money_type, &money) != GLTANG_OK
      || gltang_handle_custom_write_bytes(call, money, 0, &amount, sizeof amount) != GLTANG_OK
      || gltang_call_arg(call, 1, &currency) != GLTANG_OK
      || gltang_handle_custom_slot_set(call, money, 0, currency) != GLTANG_OK) {
    return false;
  }
  gltang_call_return_handle(call, money);
  return true;
}

int main(void) {
  int status = 1;
  GLTANG_TypeRegistry * registry = NULL;
  GLTANG_Tree * tree = NULL;
  GLTANG_Program * program = NULL;
  GRCORE_Group * group = NULL;
  GRCORE_Options * options = NULL;
  GRHEAP_Options * heap_options = NULL;
  GRCORE_Context * context = NULL;
  GRHEAP_Heap * heap = NULL;
  GLTANG_Execution * execution = NULL;
  GLTANG_Library * library = NULL;

  /* The type, once for the process. The registry copies the descriptor. */
  GLTANG_TypeDescriptor money = GLTANG_TYPE_DESCRIPTOR_INIT;
  money.name = "money";
  money.version = 1;
  money.slot_count = 1;              /* the currency */
  money.byte_count = sizeof(int64_t); /* the amount */
  money.thread_safe = true;          /* nothing in it is shared and mutable */
  money.add = money_add;
  money.less = money_less;
  money.attribute_get = money_attribute;
  if (gltang_type_registry_create(&registry) != GLTANG_OK || gltang_type_registry_add(registry, &money, &money_type) != GLTANG_OK) {
    fprintf(stderr, "the type could not be registered\n");
    goto done;
  }

  const char * source =
    "use money;\n"
    "price = money(1250, \"EUR\"); tip = 3 + money(200, \"EUR\");\n"
    "total = price + tip;\n"
    "print(total.amount); print(\" \"); print(total.currency); print(\" \"); print(price < total);\n"
    "print(\" \"); print(price == price); print(\" \"); print(price == money(1250, \"EUR\"));\n"
    "x = price + \"!\";\n"
    "x;\n";
  GLTANG_ParseError error;
  if (gltang_parse(source, GLTANG_PARSE_SCRIPT, &error, &tree) != GLTANG_OK || gltang_compile(tree, "money.tang", &error, &program) != GLTANG_OK) {
    fprintf(stderr, "refused: %d:%d: %s\n", error.line, error.column, error.message);
    goto done;
  }
  if (grcore_group_create(NULL, NULL, &group) != GRCORE_OK || grcore_options_create(NULL, &options) != GRCORE_OK || grheap_options_create(NULL, &heap_options) != GRHEAP_OK
      || gltang_heap_options_configure(heap_options) != GLTANG_OK || grcore_context_create(group, options, &context) != GRCORE_OK
      || grheap_heap_create(context, heap_options, &heap) != GRHEAP_OK || gltang_execution_create(context, program, &execution) != GLTANG_OK) {
    fprintf(stderr, "the runtime could not be made\n");
    goto done;
  }
  /* Attaching seals the registry; the execution keeps it until it is destroyed. */
  if (gltang_library_create(NULL, &library) != GLTANG_OK || gltang_library_add_native(library, "money", make_money, NULL) != GLTANG_OK
      || gltang_execution_set_libraries(execution, library) != GLTANG_OK || gltang_execution_attach_types(execution, registry) != GLTANG_OK) {
    fprintf(stderr, "the program's library could not be attached\n");
    goto done;
  }
  GRCORE_Outcome outcome;
  if (grcore_run(context, gltang_execution_entry, execution, &outcome) != GRCORE_OK || outcome != GRCORE_OUTCOME_FINISHED) {
    fprintf(stderr, "the run did not finish\n");
    goto done;
  }
  size_t length = 0;
  const char * output = gltang_execution_output_raw(execution, &length);
  printf("%.*s\n", (int)length, output);
  GLTANG_ErrorKind kind = GLTANG_ERROR_KIND_COUNT;
  /* 12.50 + (3 + 2.00) is 14.53; equality with no `equal` is identity; money
   * has no rule for a string, and the string's rule has none for money. */
  const char * expected = "1453 EUR true true false";
  if (length != strlen(expected) || memcmp(output, expected, length) != 0 || !gltang_execution_result_error(execution, &kind, NULL) || kind != GLTANG_ERROR_NOT_SUPPORTED) {
    fprintf(stderr, "the program did not compute what money says it should\n");
    goto done;
  }
  status = 0;

done:
  gltang_library_release(library);
  if (context) {
    grcore_context_destroy(context);
  }
  grheap_options_destroy(heap_options);
  grcore_options_destroy(options);
  if (group) {
    grcore_group_destroy(group);
  }
  gltang_program_release(program);
  gltang_tree_destroy(tree);
  gltang_type_registry_release(registry);
  return status;
}
