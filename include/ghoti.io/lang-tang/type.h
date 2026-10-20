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
 * attaches it to an execution with ::gltang_execution_attach_types
 * (execution.h). Attaching **seals** it: a sealed registry refuses `add`, and
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
 * assignments, which assign the answer back to the variable; an operation
 * may use it to update its left operand in place and answer it. Pairs of
 * built-in kinds behave as they always have.
 *
 * **Equality.** `==` calls `equal` and `!=` calls `not_equal`, each with
 * reflection, before any rule of the language. A `!=` that both declined asks
 * `equal` the same way and answers the opposite of a boolean answer. When
 * every operation declines, two custom values are equal only if they are the
 * same object, and a custom value is never equal to a value of another kind.
 * Map keys stay strings: a type needs no hash.
 *
 * **The other operations** (negate, attribute get and set, index get and set,
 * slice) are called on their custom operand; an empty entry, or a decline,
 * gives the error the language gives for a value with no such operation:
 * `Not implemented` for an attribute read, `Not supported` for the others.
 *
 * Iteration, calls, casts, truthiness, printing and conversion to a string are
 * not operations of this version: a custom value is truthy, `print` appends
 * nothing for it, `as` and string conversion are `Not supported`, and calling
 * one is `Invalid function call`. A snapshot of an execution that holds an
 * instance is refused (::gltang_snapshot_refused_type names the type).
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
  GLTANG_OP_ERROR     ///< Failed: the error kind set through the call object.
} GLTANG_OpStatus;

/**
 * @brief A binary operator: `+ - * / %`.
 *
 * @param call The call: answer through it, and make handles in it.
 * @param user The descriptor's `user`.
 * @param self The operand whose type this is.
 * @param other The other operand, of any kind.
 * @param self_is_lhs Whether `self` is the left operand: false for the
 *   reflected call, which computes `other OP self`.
 * @param is_assignment Whether the expression is a compound assignment (`+=`).
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
} GLTANG_TypeDescriptor;

/** @brief The smallest `size` a descriptor may state: every member of this first version. */
#define GLTANG_TYPE_DESCRIPTOR_MIN_SIZE \
  (offsetof(GLTANG_TypeDescriptor, slice) + sizeof(GLTANG_TypeSliceFn))

/**
 * @brief An initialiser that sets `size` and zeroes the rest:
 *   `GLTANG_TypeDescriptor d = GLTANG_TYPE_DESCRIPTOR_INIT;`
 */
#define GLTANG_TYPE_DESCRIPTOR_INIT {sizeof(GLTANG_TypeDescriptor), NULL, 0, 0, 0, false, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL}

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
 *   not 1 to 64 identifier characters, a slot or byte count past its maximum,
 *   or a name and version the registry already holds; ::GLTANG_ERR_OOM.
 */
GLTANG_API GLTANG_Result gltang_type_registry_add(
    GLTANG_TypeRegistry * registry, const GLTANG_TypeDescriptor * descriptor,
    const GLTANG_Type ** out_type);

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

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GLTANG_TYPE_H
