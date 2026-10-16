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
 * @file description.h
 * @stability stable
 *
 * Descriptions: a host's way of giving a Tang program a value that holds other
 * values, without touching the program's heap.
 *
 * A host that wants a template to see `menu` as an array of maps of strings
 * cannot build that in the engine's heap: the heap belongs to one execution,
 * moves its objects, and is collected while the program runs. It builds a
 * *description* of the value instead: a tree of nodes in the host's own memory
 * (null, boolean, integer, float, string, array, map), which is plain data, owns
 * no pointer into any heap, and can be built before any execution exists. The
 * engine turns a description into a real value, a fresh copy in the heap of the
 * execution that asks, each time a program reaches it.
 *
 * **Where a description is handed over** (three places, none of which holds a
 * pointer into a heap):
 *
 * - ::gltang_library_add_value: a library member whose value is the
 *   description. Each `use` that reaches it binds a fresh copy.
 * - ::gltang_library_add_value_factory: a lazy member. The engine calls the
 *   factory, with an empty description to fill, only when a `use` reaches it,
 *   never at registration.
 * - ::gltang_call_return_value: a native function's answer, a description it
 *   built during the call (::gltang_call_description) or one the host built
 *   earlier and keeps.
 *
 * **Building.** Every node is made with a `gltang_value_*` constructor, which
 * returns a ::GLTANG_ValueRef naming it, and a container takes its children
 * with ::gltang_value_array_push and ::gltang_value_map_set. A node has at most
 * one parent: attaching one that already has a parent, or one that is an
 * ancestor of the container it is attached to (which would make a cycle), is
 * refused with ::GLTANG_ERR_INVALID. The description's value is the node given
 * to ::gltang_value_set_root; with no root it is `null`.
 *
 * **Strings** carry a ::GLTANG_String_Type, which survives inside a container,
 * so that a menu label made `html` is escaped by `print` and a link `percent`
 * is encoded by it. A string or map key is validated as UTF-8 once, when it is
 * added, and refused with ::GLTANG_ERR_INVALID if it is not; nothing validates
 * it again when the value is made. A map keeps its keys in the order they were
 * set; a key set twice keeps its first position and takes the last value.
 *
 * **Limits.** A description is bounded in the number of nodes, in bytes (the
 * bytes of every string and key, and 16 for each node), and in the depth of its
 * nesting (a scalar is depth 0, an array of scalars depth 1). The defaults are
 * ::GLTANG_VALUE_DEFAULT_NODES, ::GLTANG_VALUE_DEFAULT_BYTES and
 * ::GLTANG_VALUE_DEFAULT_DEPTH; a host raises them with
 * ::GLTANG_ValueLimits, up to the hard ceilings ::GLTANG_VALUE_MAX_NODES,
 * ::GLTANG_VALUE_MAX_BYTES and ::GLTANG_VALUE_MAX_DEPTH. An addition that would
 * pass a limit is refused with ::GLTANG_ERR_LIMIT and changes nothing. The
 * engine checks the same limits again when it makes the value, whatever the
 * description says of itself.
 *
 * **Sealing and sharing.** A description is mutable until it is sealed (by
 * ::gltang_value_description_seal, or by being handed over); after that every
 * building call refuses with ::GLTANG_ERR_INVALID. A sealed description is
 * immutable and atomically reference counted: many contexts on many threads may
 * read one at the same time, and each gets its own copy.
 *
 * **Charging.** The engine charges what it does to the execution that asks: the
 * heap memory of the copy to the context's memory budget, and the work of making
 * it to the context's fuel (a long copy can run the budget out, which unwinds
 * the run like any other long operation). A description built *during a call*
 * (::gltang_call_description) is allocated from the context's own allocator, so
 * its memory is charged too, and it belongs to the call: it cannot be retained
 * and is freed when the call returns. A description built by the host before a
 * run uses the allocator it is made with (the default one).
 *
 * **Threads.** Build a description on one thread. Once sealed it may be read by
 * any number of threads and retained or released from any thread.
 */

#ifndef GHOTI_IO_GLTANG_DESCRIPTION_H
#define GHOTI_IO_GLTANG_DESCRIPTION_H

#include <ghoti.io/lang-tang/macros.h>

#include <ghoti.io/lang-tang/allocator.h>
#include <ghoti.io/lang-tang/core.h>
#include <ghoti.io/lang-tang/encoding.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The default most nodes a description may hold. */
#define GLTANG_VALUE_DEFAULT_NODES ((size_t)100000)
/** @brief The default most bytes a description may hold (16 MiB). */
#define GLTANG_VALUE_DEFAULT_BYTES ((size_t)16 * 1024 * 1024)
/** @brief The default deepest nesting a description may have. */
#define GLTANG_VALUE_DEFAULT_DEPTH ((size_t)64)

/** @brief The hard ceiling on nodes: no limit may be raised past it. */
#define GLTANG_VALUE_MAX_NODES ((size_t)4194304)
/** @brief The hard ceiling on bytes (256 MiB). */
#define GLTANG_VALUE_MAX_BYTES ((size_t)256 * 1024 * 1024)
/** @brief The hard ceiling on depth: the engine's deepest nesting of values. */
#define GLTANG_VALUE_MAX_DEPTH ((size_t)2048)

/** @brief A description of a value. Opaque; reference counted once sealed. */
typedef struct GLTANG_ValueDescription GLTANG_ValueDescription;

/**
 * @brief A handle to one node of one description.
 *
 * Valid only for the description that made it: handed to another description it
 * is refused with ::GLTANG_ERR_INVALID, as is a zero ref, which names no node.
 */
typedef struct GLTANG_ValueRef {
  uint32_t index;   ///< Which node.
  uint32_t owner;   ///< Which description made it.
} GLTANG_ValueRef;

/**
 * @brief The limits of a description. A zero field takes its default.
 *
 * Size-prefixed so the struct can grow: set `size` to `sizeof(GLTANG_ValueLimits)`.
 */
typedef struct GLTANG_ValueLimits {
  size_t size;        ///< `sizeof(GLTANG_ValueLimits)`.
  size_t max_nodes;   ///< Most nodes; at most ::GLTANG_VALUE_MAX_NODES.
  size_t max_bytes;   ///< Most bytes; at most ::GLTANG_VALUE_MAX_BYTES.
  size_t max_depth;   ///< Deepest nesting; at most ::GLTANG_VALUE_MAX_DEPTH.
} GLTANG_ValueLimits;

/**
 * @brief Makes an empty description.
 *
 * @param limits The limits, or NULL for the defaults.
 * @param out_description Receives the description, with one reference. Written
 *   only on success.
 * @return ::GLTANG_OK; ::GLTANG_ERR_INVALID for NULL `out_description` or a
 *   `limits` whose `size` is wrong; ::GLTANG_ERR_LIMIT for a limit past its
 *   ceiling; ::GLTANG_ERR_OOM.
 */
GLTANG_API GLTANG_Result gltang_value_description_create(
    const GLTANG_ValueLimits * limits,
    GLTANG_ValueDescription ** out_description);

/**
 * @brief Takes another reference.
 *
 * @param description The description, or NULL.
 * @return `description`; NULL for NULL, and for a description built during a
 *   call, which belongs to the call and cannot be kept.
 */
GLTANG_API GLTANG_ValueDescription * gltang_value_description_retain(
    GLTANG_ValueDescription * description);

/**
 * @brief Gives a reference back; the last one frees the description.
 *
 * @param description The description, or NULL (ignored). A description built
 *   during a call is ignored too: the call frees it.
 */
GLTANG_API void gltang_value_description_release(
    GLTANG_ValueDescription * description);

/**
 * @brief Seals the description: it can no longer change.
 *
 * Handing a description over seals it; this is for a host that wants to share
 * one before then. Sealing twice is not an error.
 *
 * @param description The description.
 * @return ::GLTANG_OK; ::GLTANG_ERR_INVALID for NULL.
 */
GLTANG_API GLTANG_Result gltang_value_description_seal(
    GLTANG_ValueDescription * description);

/** @brief Whether the description is sealed. False for NULL. */
GLTANG_API bool gltang_value_description_sealed(
    const GLTANG_ValueDescription * description);

/**
 * @brief The host context of the execution a description was made for: the
 *   description a value factory (::gltang_library_add_value_factory) is handed.
 *
 * @param description The description.
 * @return The pointer given to ::gltang_execution_set_host_context, or NULL
 *   for a description the host made itself.
 */
GLTANG_API void * gltang_description_host_context(
    const GLTANG_ValueDescription * description);

/** @brief How many nodes the description holds so far; 0 for NULL. */
GLTANG_API size_t gltang_value_description_nodes(
    const GLTANG_ValueDescription * description);

/** @brief How many bytes the description counts against its limit so far; 0 for NULL. */
GLTANG_API size_t gltang_value_description_bytes(
    const GLTANG_ValueDescription * description);

/**
 * @name Making nodes
 *
 * Each call makes one node, not yet attached to anything, and writes its
 * handle to `out_ref` only on success. Each refuses, changing nothing, with
 * ::GLTANG_ERR_INVALID for a NULL argument or a sealed description,
 * ::GLTANG_ERR_LIMIT when the node or byte limit would be passed, and
 * ::GLTANG_ERR_OOM when memory runs out.
 * @{
 */

/** @brief Makes `null`. */
GLTANG_API GLTANG_Result gltang_value_null(
    GLTANG_ValueDescription * description, GLTANG_ValueRef * out_ref);

/** @brief Makes a boolean. */
GLTANG_API GLTANG_Result gltang_value_bool(
    GLTANG_ValueDescription * description, bool value, GLTANG_ValueRef * out_ref);

/** @brief Makes an integer. */
GLTANG_API GLTANG_Result gltang_value_integer(
    GLTANG_ValueDescription * description, int64_t value,
    GLTANG_ValueRef * out_ref);

/** @brief Makes a float. */
GLTANG_API GLTANG_Result gltang_value_float(
    GLTANG_ValueDescription * description, double value,
    GLTANG_ValueRef * out_ref);

/**
 * @brief Makes a string, copied.
 *
 * @param text UTF-8, `length` bytes; need not be NUL-terminated. Text that is
 *   not valid UTF-8 is refused with ::GLTANG_ERR_INVALID.
 * @param length The byte count.
 * @param encoding The segment tag the string carries (an out-of-range tag is
 *   ::GLTANG_ERR_INVALID).
 */
GLTANG_API GLTANG_Result gltang_value_string(
    GLTANG_ValueDescription * description, const char * text, size_t length,
    GLTANG_String_Type encoding, GLTANG_ValueRef * out_ref);

/** @brief Makes an empty array. */
GLTANG_API GLTANG_Result gltang_value_array(
    GLTANG_ValueDescription * description, GLTANG_ValueRef * out_ref);

/** @brief Makes an empty map. */
GLTANG_API GLTANG_Result gltang_value_map(
    GLTANG_ValueDescription * description, GLTANG_ValueRef * out_ref);

/** @} */

/**
 * @name Attaching nodes
 *
 * Each call refuses, changing nothing, with ::GLTANG_ERR_INVALID for a NULL
 * argument, a sealed description, a handle that names no node or the wrong kind
 * of node, a child that already has a parent (or is the root), or a child that
 * is the container itself or one of its ancestors; and with ::GLTANG_ERR_LIMIT
 * when the depth or byte limit would be passed.
 * @{
 */

/** @brief Appends `element` to `array`. */
GLTANG_API GLTANG_Result gltang_value_array_push(
    GLTANG_ValueDescription * description, GLTANG_ValueRef array,
    GLTANG_ValueRef element);

/**
 * @brief Sets `key` in `map` to `value`.
 *
 * A key set twice keeps its first position and takes the last value.
 *
 * @param key UTF-8, `key_length` bytes; refused with ::GLTANG_ERR_INVALID if it
 *   is not valid UTF-8. Keys are matched by their bytes only, whatever their tag.
 * @param key_encoding The tag the key string carries.
 */
GLTANG_API GLTANG_Result gltang_value_map_set(
    GLTANG_ValueDescription * description, GLTANG_ValueRef map,
    const char * key, size_t key_length, GLTANG_String_Type key_encoding,
    GLTANG_ValueRef value);

/**
 * @brief Makes `value` the description's value.
 *
 * The node must have no parent. A description whose root is never set is `null`.
 */
GLTANG_API GLTANG_Result gltang_value_set_root(
    GLTANG_ValueDescription * description, GLTANG_ValueRef value);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GLTANG_DESCRIPTION_H */
