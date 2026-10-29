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
 * @file type.h
 * @stability stable
 *
 * Host-defined value types: a `money`, an `account`, whose values answer `+`,
 * `<`, `==`, `a.b`, `a[i]` and `a[i:j]` the way the host says.
 *
 * **One kind, many types.** Every host-defined value is of the one kind
 * ::GLTANG_KIND_CUSTOM. An *instance* is one object in the execution's heap:
 * a pointer to its type, a fixed number of **value slots** and a fixed number
 * of **raw bytes**, both fixed by the type and zero-filled when it is made. A
 * slot holds any value of the program (an array, a map, another instance),
 * and the collector sees it: an instance keeps what its slots hold alive, and
 * a cycle through slots is collected like any other. The raw bytes are plain
 * data the collector never looks at. Nothing else of an instance holds a
 * reference, so a type has no trace function to get wrong. An instance is
 * made, and its slots and bytes read and written, through the handle API
 * (handle.h: ::gltang_handle_make_custom and the `gltang_handle_custom_*`
 * calls), and it is passed by reference, as a function is: storing it in an
 * array stores the same object.
 *
 * **The descriptor** (::GLTANG_TypeDescriptor) says what a type is: its name
 * and version, its slot and byte counts, whether it is thread-safe, a `user`
 * pointer shared by every instance, and its *operation table*. Every operation
 * is optional. The registry copies the descriptor when it is added, so the
 * host's struct need not outlive the call.
 *
 * **The registry** (::GLTANG_TypeRegistry) holds descriptors. The host
 * creates it, adds each descriptor with ::gltang_type_registry_add (which
 * gives the ::GLTANG_Type, the handle a host makes instances with), and
 * attaches it to an execution with ::gltang_execution_attach_types. Attaching **seals** it: a sealed registry refuses `add`, and
 * is read by every execution and thread with no lock. The execution retains it
 * for its whole life. A registry holding a type that is not `thread_safe` is
 * **bound** to the first context it is attached to, and attaching it to an
 * execution of any other context is refused.
 *
 * **Identity.** A type is the same type as another only if its registry, its
 * name and its version are all the same: two registries that each hold a
 * `money` version 1 hold two types. A registry's id is unique in the process
 * and never reused. A type's *table hash* covers what it declares (name,
 * version, slot and byte counts, `thread_safe` and which operations it
 * implements), never an address.
 *
 * **Operations.** An operation is a host callback, called as a native is
 * (handle.h): it is given the call object and handles, it reads and makes
 * values through the handle API, it is charged fuel as a native call plus the
 * bytes it answers, it takes a unit of native depth, it may poll the runtime
 * with ::gltang_call_poll, and it runs as host code, so it cannot destroy or
 * reconfigure the execution. It answers with one of the ::GLTANG_OpStatus
 * values:
 *  - ::GLTANG_OP_OK: the answer is the value set with a
 *    `gltang_call_return_*` call (::gltang_call_return_handle for any value),
 *    null when none was set;
 *  - ::GLTANG_OP_DECLINE: this type has no answer for these operands. A
 *    decline is a status and nothing else: no value is made, nothing is
 *    allocated, no error is logged;
 *  - ::GLTANG_OP_ERROR: the answer is the error value of the kind set with
 *    ::gltang_call_return_error (`Host function failed` when none was set).
 *
 * A handle or an allocation that cannot be made, or a poll that stops the
 * run, makes the operation's value `Out of memory` or stops the run, exactly
 * as for a native, whatever it returns.
 *
 * **Binary operators and comparisons, with reflection.** When either operand
 * of `+ - * / %`, `< <= > >=` or `== !=` is a custom value:
 *  0. except for `==` and `!=`, if the other operand is an error, that error
 *     is the value and no operation is called (as a string's `+` gives its
 *     error operand), so `money + (1 / 0)` is `Divide by zero`;
 *  1. if the left operand is custom, its operation is called with `self` the
 *     left, `other` the right and `self_is_lhs` true;
 *  2. if that declines (or the left is not custom) and the right operand is
 *     custom, the *same* operation of the right operand is called with `self`
 *     the right, `other` the left and `self_is_lhs` false, and it computes
 *     `other OP self`;
 *  3. if that declines too, the value is the language's `Not supported`
 *     error, made then and only then.
 *
 * So `3 + money` and `money + 3` both reach `money`'s add, with `self_is_lhs`
 * false and true. A built-in left operand has no rule for a custom right
 * operand in this version: `"x" + money` is `money`'s reflected add, never an
 * empty string. `is_assignment` is true for `+=` and the other compound
 * assignments, which assign the answer back to the variable on the left: the
 * assignment's target is the left operand, so it is `self` only when
 * `self_is_lhs` is true (a reflected call of `n += money` gets `is_assignment`
 * true with `self` the right operand, which is not assigned to). An instance
 * is shared by every variable and container that holds it, so an operation
 * that updates its operand in place changes what all of them see; one that
 * must not makes a new instance. Pairs of built-in kinds behave as they
 * always have.
 *
 * **Equality.** `==` calls `equal` and `!=` calls `not_equal`, each with
 * reflection, before any rule of the language. A `!=` that both declined asks
 * `equal` the same way and is false only when `equal` answers exactly `true`. When
 * every operation declines, two custom values are equal only if they are the
 * same object, and a custom value is never equal to a value of another kind.
 * Inside an array comparison an element pair with a custom side asks `equal`
 * the same way: only `true` makes the elements equal, and an error it answers
 * is the value of the whole comparison, as it is at the top level.
 * Map keys stay strings: a type needs no hash.
 *
 * **The other operations** (negate, attribute get and set, index get and set,
 * slice) are called on their custom operand; an empty entry, or a decline,
 * gives the error the language gives for a value with no such operation:
 * `Not implemented` for an attribute read, `Not supported` for the others.
 *
 * **Iteration.** `for (x : v)` over a custom `v` calls `iterator_begin(v)`.
 * Its answer drives the loop: a custom value whose type has `iterator_next`
 * (typically a new instance; `v` itself is allowed), or an array, which is
 * iterated as an array is. Anything else, a missing `iterator_begin` and a
 * decline are `Not supported`, the loop's value. Each step calls
 * `iterator_next(iterator)`: ::GLTANG_OP_OK with the item set yields it (null
 * when none was set), ::GLTANG_OP_DONE ends the loop, an error answer ends it
 * and is the loop's value, and a decline ends it with `Not supported`. The
 * iterator is held by the loop, so the collector sees it.
 *
 * **Calls.** `v(a, b)` calls `call(v, args, argc)` with the arguments as
 * handles; its answer is the value. A missing entry or a decline is
 * `Invalid function call`. A callable instance is a value, not a function: it
 * cannot be given where only a function is accepted.
 *
 * **Casts.** `v as int|float|bool|string` calls `cast(v, target)` first when
 * the type has one. A decline falls to the defaults: `bool` is the
 * truthiness below, `string` is `to_string`, and `int` and `float` are
 * `Not supported`. A cast to `string` of an instance with neither `cast` nor
 * `to_string` is `Not supported`.
 *
 * **Truthiness.** `truthy` sets `*out_truthy`; a decline, or no entry, is
 * true. An instance marked as standing for an error
 * (::gltang_handle_custom_set_error) is falsy and its type is not asked, in
 * `as bool` too. When `truthy` answers an error (::GLTANG_OP_ERROR, or an error
 * set through the call object), the error is falsy, and each site does one of
 * two things with it. It is the *value* of `!v`, of `v && x` (in place of `v`)
 * and of `v as bool`. It is *treated as false and recorded as discarded* (as
 * an expression statement's error value is, ::GLTANG_ERROR_HOW_DISCARDED) in
 * `if`, `while`, `do ... while`, `?:` and as the left side of `v || x`, where
 * it is the value of nothing.
 *
 * **Printing.** `print` answers a string that is appended to the output with
 * each segment's own tag, so it is escaped per segment exactly as a string's
 * is. `to_string` answers the string used for `as string`, for the element
 * rendering of an instance inside an array or map, and (when there is no
 * `print`) for `print`. With neither, `print` appends nothing and an element
 * shows `Custom: name`. An answer that is not a string is
 * `Host function failed`, and nothing partial is appended.
 *
 * **Standing for an error.** ::gltang_handle_custom_set_error marks one
 * instance. It is falsy, and where an engine error value contaminates (an
 * operand of an operator, a function's result, a statement's value) it
 * does too. It is *not* an engine error: it is never in the error list, never
 * fires the error-created hook, and has no error kind.
 *
 * **Attributes on built-in kinds.** ::gltang_type_registry_add_attribute adds
 * `s.slug` to strings (and to integers, floats, booleans, arrays, functions
 * and random generators). A built-in name always wins; maps and libraries
 * take none, since their names are the user's keys and members.
 *
 * **Lifetimes.** The handles an operation is given (`self`, the operands, a
 * call's `args`) are valid until the callback returns and no longer; to keep a
 * value, store it in a slot (::gltang_handle_custom_slot_set) or promote it
 * (::gltang_handle_promote). An answer set with ::gltang_call_return_handle is
 * read when the callback returns, and an iterator's item is copied into the
 * loop's variable then: the handle need not outlive the call.
 * `iterator_begin` runs once each time the loop is *entered*, so a loop nested
 * in another calls it once per iteration of the outer one; each entry is a new
 * iterator. A callback gets its operands in a copy held by the call; up to
 * sixteen operands beside `self` (a call's arguments) the copy is in the
 * engine's frame, and past that it is a transient block of host memory (cutil's
 * allocator, not the execution's heap), made even when the callback declines,
 * so that the decline path of such a call makes allocations but no heap object.
 *
 * **A marked instance as the operand of an operation.** An instance marked as
 * standing for an error is an error value only where the rules above say so:
 * falsy, and contaminating as an operand of an operator, in assignment, as a
 * function's result and as a statement's value. It is an ordinary instance
 * everywhere else: as the source of a `for` it is asked for its
 * `iterator_begin`, as a callee for its `call`, and as an operand of
 * `as int|float|string` for its `cast` (and `to_string`); only `as bool` is
 * false without asking.
 *
 * A snapshot of an execution that holds an instance is refused
 * (::gltang_snapshot_refused_type names the type).
 *
 * **Threads.** A registry is built on one thread. Once sealed, it and its
 * descriptors may be read from any thread. ::gltang_type_registry_retain and
 * ::gltang_type_registry_release are safe from any thread. An instance
 * belongs to one execution.
 */

#ifndef GHOTI_IO_GLTANG_TYPE_H
#define GHOTI_IO_GLTANG_TYPE_H

#include <ghoti.io/lang-tang/macros.h>

#include <ghoti.io/lang-tang/core.h>
#include <ghoti.io/lang-tang/handle.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The longest a type's name is, in bytes. */
#define GLTANG_TYPE_NAME_MAX 64u

/** @brief The most value slots an instance has. */
#define GLTANG_TYPE_SLOTS_MAX 65535u

/** @brief The most raw bytes an instance has (1 MiB). */
#define GLTANG_TYPE_BYTES_MAX 1048576u

/** @brief A set of descriptors, sealed when it is attached. Opaque; reference counted. */
typedef struct GLTANG_TypeRegistry GLTANG_TypeRegistry;

/** @brief What an operation answers. */
typedef enum GLTANG_OpStatus {
  GLTANG_OP_OK = 0,   ///< Answered: the value set through the call object.
  GLTANG_OP_DECLINE,  ///< No answer for these operands: the next rule is tried.
  GLTANG_OP_ERROR,    ///< Failed: the error kind set through the call object.
  GLTANG_OP_DONE      ///< No more items: valid only from an iterator's `iterator_next`; from any other operation it is `Host function failed`.
} GLTANG_OpStatus;

/** @brief What a cast converts to (the target of a type's `cast`). */
typedef enum GLTANG_CastTarget {
  GLTANG_CAST_TARGET_INT = 0,  ///< `v as int`
  GLTANG_CAST_TARGET_FLOAT,    ///< `v as float`
  GLTANG_CAST_TARGET_BOOL,     ///< `v as bool`
  GLTANG_CAST_TARGET_STRING    ///< `v as string`
} GLTANG_CastTarget;

/**
 * @brief A binary operator: `+ - * / %`.
 *
 * @param call The call: answer through it, and make handles in it.
 * @param user The descriptor's `user`.
 * @param self The operand whose type this is.
 * @param other The other operand, of any kind.
 * @param self_is_lhs Whether `self` is the left operand: false for the
 *   reflected call, which computes `other OP self`.
 * @param is_assignment Whether the expression is a compound assignment (`+=`),
 *   whose target is the left operand (`self` only when `self_is_lhs`).
 */
typedef GLTANG_OpStatus (*GLTANG_TypeBinaryFn)(GLTANG_NativeCall * call,
    void * user, GLTANG_Handle self, GLTANG_Handle other, bool self_is_lhs,
    bool is_assignment);

/** @brief A comparison: `< <= > >= == !=`; as ::GLTANG_TypeBinaryFn. */
typedef GLTANG_OpStatus (*GLTANG_TypeCompareFn)(GLTANG_NativeCall * call,
    void * user, GLTANG_Handle self, GLTANG_Handle other, bool self_is_lhs);

/** @brief A unary operator: `-self`. */
typedef GLTANG_OpStatus (*GLTANG_TypeUnaryFn)(GLTANG_NativeCall * call,
    void * user, GLTANG_Handle self);

/** @brief `self.name`; `name` is a string. */
typedef GLTANG_OpStatus (*GLTANG_TypeAttrGetFn)(GLTANG_NativeCall * call,
    void * user, GLTANG_Handle self, GLTANG_Handle name);

/**
 * @brief `self.name = value`. With no answer set, the expression's value is
 *   `value`, as for any assignment.
 */
typedef GLTANG_OpStatus (*GLTANG_TypeAttrSetFn)(GLTANG_NativeCall * call,
    void * user, GLTANG_Handle self, GLTANG_Handle name, GLTANG_Handle value);

/** @brief `self[index]`; the index is any value. */
typedef GLTANG_OpStatus (*GLTANG_TypeIndexGetFn)(GLTANG_NativeCall * call,
    void * user, GLTANG_Handle self, GLTANG_Handle index);

/**
 * @brief `self[index] = value`. With no answer set, the expression's value is
 *   `value`.
 */
typedef GLTANG_OpStatus (*GLTANG_TypeIndexSetFn)(GLTANG_NativeCall * call,
    void * user, GLTANG_Handle self, GLTANG_Handle index, GLTANG_Handle value);

/**
 * @brief `self[start:stop:step]`. A part the source leaves out is null, as a
 *   part that is null is.
 */
typedef GLTANG_OpStatus (*GLTANG_TypeSliceFn)(GLTANG_NativeCall * call,
    void * user, GLTANG_Handle self, GLTANG_Handle start, GLTANG_Handle stop,
    GLTANG_Handle step);

/**
 * @brief `for (x : self)`: starts an iteration. The answer is the iterator: a
 *   custom value whose type has `iterator_next` (often a new instance; `self`
 *   itself is allowed) or an array. Any other answer is `Not supported`.
 */
typedef GLTANG_OpStatus (*GLTANG_TypeIteratorBeginFn)(GLTANG_NativeCall * call,
    void * user, GLTANG_Handle self);

/**
 * @brief One step of an iteration; `self` is the iterator ::GLTANG_TypeIteratorBeginFn
 *   answered. ::GLTANG_OP_OK with the item set yields it (null when none was
 *   set), ::GLTANG_OP_DONE ends the loop, ::GLTANG_OP_ERROR ends it with the
 *   error as the loop's value, and ::GLTANG_OP_DECLINE ends it with
 *   `Not supported`.
 */
typedef GLTANG_OpStatus (*GLTANG_TypeIteratorNextFn)(GLTANG_NativeCall * call,
    void * user, GLTANG_Handle self);

/**
 * @brief `self(args...)`. `args` are handles in the callback's scope.
 */
typedef GLTANG_OpStatus (*GLTANG_TypeCallFn)(GLTANG_NativeCall * call,
    void * user, GLTANG_Handle self, const GLTANG_Handle * args, size_t argc);

/** @brief `self as int|float|bool|string`; a decline falls to the defaults. */
typedef GLTANG_OpStatus (*GLTANG_TypeCastFn)(GLTANG_NativeCall * call,
    void * user, GLTANG_Handle self, GLTANG_CastTarget target);

/**
 * @brief Truthiness: set `*out_truthy` and answer ::GLTANG_OP_OK. A decline is
 *   true. `*out_truthy` starts true.
 */
typedef GLTANG_OpStatus (*GLTANG_TypeTruthyFn)(GLTANG_NativeCall * call,
    void * user, GLTANG_Handle self, bool * out_truthy);

/**
 * @brief `print self`: the answer is a string (any encoding, any segments)
 *   appended to the output with each segment's own tag.
 */
typedef GLTANG_OpStatus (*GLTANG_TypePrintFn)(GLTANG_NativeCall * call,
    void * user, GLTANG_Handle self);

/**
 * @brief The type's text: the answer is a string. Used for `as string`, for
 *   the element rendering inside an array or map, and by `print` when the
 *   type has no `print`.
 */
typedef GLTANG_OpStatus (*GLTANG_TypeToStringFn)(GLTANG_NativeCall * call,
    void * user, GLTANG_Handle self);

/**
 * @brief An attribute a host adds to a built-in kind (`s.slug`): the answer
 *   is the attribute's value. A decline is `Not implemented`.
 */
typedef GLTANG_OpStatus (*GLTANG_HostAttributeFn)(GLTANG_NativeCall * call,
    void * user, GLTANG_Handle self);

/**
 * @brief What a type is. Size-prefixed: set `size` to
 *   `sizeof(GLTANG_TypeDescriptor)` (::GLTANG_TYPE_DESCRIPTOR_INIT does), and
 *   zero what you do not use.
 *
 * Members are added only at the end. A registry reads a member only when
 * `size` covers it, so a module compiled against this header keeps working
 * with a later engine, the members it does not know being empty.
 */
typedef struct GLTANG_TypeDescriptor {
  size_t size;                   ///< `sizeof(GLTANG_TypeDescriptor)` as the host compiled it.
  const char * name;             ///< 1 to ::GLTANG_TYPE_NAME_MAX of `[A-Za-z0-9_]`, not starting with a digit. Copied.
  uint32_t version;              ///< The type's version: with the name, what a type is called.
  uint32_t slot_count;           ///< Value slots per instance, at most ::GLTANG_TYPE_SLOTS_MAX.
  uint32_t byte_count;           ///< Raw bytes per instance, at most ::GLTANG_TYPE_BYTES_MAX.
  bool thread_safe;              ///< Its operations and `user` may be used from several threads at once.
  void * user;                   ///< Given to every operation; immutable once attached, or synchronised by the host.
  GLTANG_TypeBinaryFn add;       ///< `+`
  GLTANG_TypeBinaryFn subtract;  ///< `-`
  GLTANG_TypeBinaryFn multiply;  ///< `*`
  GLTANG_TypeBinaryFn divide;    ///< `/`
  GLTANG_TypeBinaryFn modulo;    ///< `%`
  GLTANG_TypeUnaryFn negate;     ///< unary `-`
  GLTANG_TypeCompareFn less;           ///< `<`
  GLTANG_TypeCompareFn less_equal;     ///< `<=`
  GLTANG_TypeCompareFn greater;        ///< `>`
  GLTANG_TypeCompareFn greater_equal;  ///< `>=`
  GLTANG_TypeCompareFn equal;          ///< `==`
  GLTANG_TypeCompareFn not_equal;      ///< `!=`
  GLTANG_TypeAttrGetFn attribute_get;  ///< `self.name`
  GLTANG_TypeAttrSetFn attribute_set;  ///< `self.name = value`
  GLTANG_TypeIndexGetFn index_get;     ///< `self[index]`
  GLTANG_TypeIndexSetFn index_set;     ///< `self[index] = value`
  GLTANG_TypeSliceFn slice;            ///< `self[start:stop:step]`
  // Added after the first version (story 6); a descriptor sized without them leaves them empty.
  GLTANG_TypeIteratorBeginFn iterator_begin;  ///< `for (x : self)`
  GLTANG_TypeIteratorNextFn iterator_next;    ///< one step of an iterator of this type
  GLTANG_TypeCallFn call;                     ///< `self(args...)`
  GLTANG_TypeCastFn cast;                     ///< `self as int|float|bool|string`
  GLTANG_TypeTruthyFn truthy;                 ///< `if (self)`, `!self`, `self && x`, `self as bool`
  GLTANG_TypePrintFn print;                   ///< `print self`
  GLTANG_TypeToStringFn to_string;            ///< `self as string`, an element's text
} GLTANG_TypeDescriptor;

/** @brief The smallest `size` a descriptor may state: every member of this first version. */
#define GLTANG_TYPE_DESCRIPTOR_MIN_SIZE \
  (offsetof(GLTANG_TypeDescriptor, slice) + sizeof(GLTANG_TypeSliceFn))

/**
 * @brief An initialiser that sets `size` and zeroes the rest:
 *   `GLTANG_TypeDescriptor d = GLTANG_TYPE_DESCRIPTOR_INIT;`
 *
 * Every member is listed, by position, because the shorter forms are refused by
 * a host built with `-Wextra -Werror`: `{sizeof(...)}` and C++20's
 * `{.size = ...}` both draw `-Wmissing-field-initializers` from g++ (C accepts
 * the designated form, C++17 has none). A member added later is added here too.
 */
#define GLTANG_TYPE_DESCRIPTOR_INIT {sizeof(GLTANG_TypeDescriptor), NULL, 0, 0, 0, false, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL}

/**
 * @brief Creates an empty registry with a reference count of one.
 *
 * @param out_registry Receives it. Written only on success.
 * @return ::GLTANG_OK; ::GLTANG_ERR_INVALID for NULL; ::GLTANG_ERR_OOM.
 */
GLTANG_API GLTANG_Result gltang_type_registry_create(
    GLTANG_TypeRegistry ** out_registry);

/** @brief Adds a reference. Safe from any thread. NULL is a no-op. */
GLTANG_API GLTANG_TypeRegistry * gltang_type_registry_retain(
    GLTANG_TypeRegistry * registry);

/**
 * @brief Drops a reference; the last one frees the registry and its types.
 *   Safe from any thread. NULL is a no-op.
 */
GLTANG_API void gltang_type_registry_release(GLTANG_TypeRegistry * registry);

/**
 * @brief Adds a type: copies the descriptor, and gives the type.
 *
 * @param registry An unsealed registry.
 * @param descriptor The descriptor; read during the call only.
 * @param out_type Receives the type, valid while the registry lives; may be
 *   NULL. Written only on success.
 * @return ::GLTANG_OK; ::GLTANG_ERR_INVALID for a NULL argument, a sealed
 *   registry, a `size` below ::GLTANG_TYPE_DESCRIPTOR_MIN_SIZE, a name that is
 *   not 1 to ::GLTANG_TYPE_NAME_MAX identifier characters, a slot or byte count past its maximum,
 *   or a name and version the registry already holds; ::GLTANG_ERR_OOM.
 */
GLTANG_API GLTANG_Result gltang_type_registry_add(
    GLTANG_TypeRegistry * registry, const GLTANG_TypeDescriptor * descriptor,
    const GLTANG_Type ** out_type);

/**
 * @brief Adds an attribute to a built-in kind: `value.name` reads as `fn`
 *   answers (`s.slug`). Get only.
 *
 * A built-in name always wins (`s.html`, `s.length`, `a.size`, the members of a
 * random generator), so a module cannot change what a built-in means; the
 * function is reached only for a name the kind does not have. The functions
 * are called as a type's operations are, and a decline is `Not implemented`.
 * A sealed registry is read by every execution that attached it, on any
 * thread, with no lock, so `fn` and `user` must be usable from several threads
 * at once (an attribute has no `thread_safe` flag of its own: the registry is
 * not bound to a context by it).
 *
 * @param registry An unsealed registry.
 * @param kind ::GLTANG_KIND_INTEGER, ::GLTANG_KIND_FLOAT, ::GLTANG_KIND_BOOL,
 *   ::GLTANG_KIND_STRING, ::GLTANG_KIND_ARRAY, ::GLTANG_KIND_FUNCTION or
 *   ::GLTANG_KIND_RNG.
 * @param name 1 to ::GLTANG_TYPE_NAME_MAX identifier characters; copied.
 * @param fn The attribute.
 * @param user Given to `fn`.
 * @return ::GLTANG_OK; ::GLTANG_ERR_INVALID for a NULL argument, a sealed
 *   registry, a kind other than the above (a map's names are its keys, a
 *   library's are its members, and a custom value's type has its own
 *   `attribute_get`), a bad name, or a kind and name the registry holds
 *   already; ::GLTANG_ERR_OOM. Attaching a registry whose kind and name an
 *   attached registry holds is refused by ::gltang_execution_attach_types.
 */
GLTANG_API GLTANG_Result gltang_type_registry_add_attribute(
    GLTANG_TypeRegistry * registry, GLTANG_ValueKind kind, const char * name,
    GLTANG_HostAttributeFn fn, void * user);

/** @brief Whether the registry is sealed (it has been attached). */
GLTANG_API bool gltang_type_registry_sealed(const GLTANG_TypeRegistry * registry);

/** @brief The registry's id: unique in the process, never reused, never 0. 0 for NULL. */
GLTANG_API uint64_t gltang_type_registry_id(const GLTANG_TypeRegistry * registry);

/** @brief How many types the registry holds. */
GLTANG_API size_t gltang_type_registry_count(const GLTANG_TypeRegistry * registry);

/**
 * @brief The type of a name and version in the registry, or NULL.
 */
GLTANG_API const GLTANG_Type * gltang_type_registry_find(
    const GLTANG_TypeRegistry * registry, const char * name, uint32_t version);

/** @brief The type's name (the registry's copy); NULL for NULL. */
GLTANG_API const char * gltang_type_name(const GLTANG_Type * type);

/** @brief The type's version; 0 for NULL. */
GLTANG_API uint32_t gltang_type_version(const GLTANG_Type * type);

/** @brief The registry the type is in; NULL for NULL. */
GLTANG_API const GLTANG_TypeRegistry * gltang_type_registry(const GLTANG_Type * type);

/** @brief The descriptor's `user`; NULL for NULL. */
GLTANG_API void * gltang_type_user(const GLTANG_Type * type);

/** @brief The type's slot count; 0 for NULL. */
GLTANG_API uint32_t gltang_type_slot_count(const GLTANG_Type * type);

/** @brief The type's raw-byte count; 0 for NULL. */
GLTANG_API uint32_t gltang_type_byte_count(const GLTANG_Type * type);

/**
 * @brief The type's table hash: a function of its name, version, slot count,
 *   byte count, `thread_safe` and the set of operations it implements. Never
 *   of an address, so the same descriptor gives the same hash in every process.
 */
GLTANG_API uint64_t gltang_type_table_hash(const GLTANG_Type * type);

/**
 * @brief Whether two types are the same type: the same registry id, name and
 *   version. Two NULLs are the same; NULL and a type are not.
 */
GLTANG_API bool gltang_type_same(const GLTANG_Type * a, const GLTANG_Type * b);

/** @brief A program running on a context (execution.h). Opaque. */
typedef struct GLTANG_Execution GLTANG_Execution;

/**
 * @brief Attaches a registry to an execution: the program's
 *   values may then be instances of its types.
 *
 * The registry is sealed (it refuses ::gltang_type_registry_add from now on)
 * and retained until the execution is destroyed. An execution may attach
 * several; attaching one twice is a no-op. A registry that holds a type that
 * is not `thread_safe` is bound to the context of the first execution it is
 * attached to, and attaching it to an execution of another context is
 * refused. Two registries that hold the same name and version with different
 * table hashes cannot both be attached to one execution.
 *
 * @param execution The execution.
 * @param registry The registry.
 * @return ::GLTANG_OK; ::GLTANG_ERR_INVALID for a NULL argument, an execution
 *   that has started, a call from inside a host function, a registry bound to another context, or a name and
 *   version that clash with a registry already attached, or a built-in kind's
 *   attribute an attached registry already holds; ::GLTANG_ERR_OOM.
 */
GLTANG_API GLTANG_Result gltang_execution_attach_types(
    GLTANG_Execution * execution, GLTANG_TypeRegistry * registry);

/**
 * @brief The type that made the last ::gltang_snapshot_take of this
 *   execution refuse, or NULL.
 *
 * An instance of a host-defined type cannot be in a snapshot yet: a take of an
 * execution whose heap holds one live is refused with
 * ::GLTANG_ERR_UNSUPPORTED, and this names the type of the first instance the
 * take met. A later take that is not refused for a type clears it.
 */
GLTANG_API const GLTANG_Type * gltang_snapshot_refused_type(
    const GLTANG_Execution * execution);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GLTANG_TYPE_H
