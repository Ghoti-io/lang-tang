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
 * @file handle.h
 * @stability stable
 *
 * Handles: how a native function or a factory touches the program's values
 * while the program runs.
 *
 * A value lives in the garbage-collected heap of one execution, and the heap
 * moves objects and collects at any allocation, so a host never holds a pointer
 * to one. It holds a *handle*: a small index into a **handle scope**, an array
 * of values the collector reads as roots and rewrites when it moves an object.
 * A handle therefore stays valid across a collection and a relocation, and it
 * ends when the callback returns.
 *
 * **Scopes.** A callback gets a scope the first time it asks for a handle
 * (::gltang_call_arg, a constructor, ::gltang_call_bound); a callback that uses
 * only the scalar calls of library.h never gets one and pays nothing. A handle
 * carries the serial of the scope it belongs to, which is checked on every call:
 * a handle of a callback that has returned, or of another callback, is refused
 * and never read, and a serial is never reused within an execution. A scope
 * holds at most as many handles as the execution's limit
 * (::gltang_execution_set_handle_limits, default 1,024); a handle that cannot be
 * made (the limit, or the context's memory) makes the callback's value
 * `Out of memory` and the run goes on.
 *
 * **Reading.** The readers take a handle and give its kind, its scalar, its
 * string's bytes **with the encoding tag of every segment**, the length and
 * elements of an array and the members of a map (an element is another handle).
 * A reader of a handle that is not valid, or not of the kind asked, gives a zero
 * value and never reads anything it was not given.
 *
 * **Creating.** The constructors make any value in the program's heap (scalars,
 * tagged strings, arrays, maps, a library, a native function bound to a value)
 * and give a new handle. Storing a container into a container copies it, as
 * the language does for `a[i] = b`, so build from the outside in when the
 * containers are large. An answer is a handle: ::gltang_call_return_handle.
 *
 * **Which calls collect.** Every function below is labelled `@may_gc` or
 * `@no_gc` in its documentation. A `@may_gc` function can run a collection (it
 * allocates in the heap, or polls the runtime); a `@no_gc` function never does.
 * A `@no_gc` function is checked against a run that collects at every
 * allocation. **A pointer returned by ::gltang_handle_text is valid until the
 * next `@may_gc` call or the end of the callback**, whichever comes first;
 * ::gltang_handle_copy_text copies out for anything longer. A host never keeps
 * a pointer across a `@may_gc` call.
 *
 * **Persistent handles.** A handle ends with its callback. A value that must
 * outlive one is promoted (::gltang_handle_promote) to a *persistent* handle:
 * rooted, kept up to date when the heap moves, counted against the execution's
 * limit (default 4,096), charged to its memory, released with
 * ::gltang_persistent_release or when the execution is destroyed. A persistent
 * handle that is released is refused. A host that keeps one in memory of its own
 * has made the value uncollectable for as long as it does: that is the cost of
 * keeping a value, and the reason it is explicit.
 *
 * **Threads.** A call object and its handles belong to the thread that runs the
 * callback. Nothing here is thread-safe, and nothing needs to be: a context has
 * one owner thread.
 */

#ifndef GHOTI_IO_GLTANG_HANDLE_H
#define GHOTI_IO_GLTANG_HANDLE_H

#include <ghoti.io/lang-tang/macros.h>

#include <ghoti.io/lang-tang/core.h>
#include <ghoti.io/lang-tang/encoding.h>
#include <ghoti.io/lang-tang/valuekind.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A named table of members. Opaque; reference counted (library.h). */
typedef struct GLTANG_Library GLTANG_Library;

/** @brief A host-defined value type, held by its registry (type.h). Opaque. */
typedef struct GLTANG_Type GLTANG_Type;

/** @brief One entry of a table of continuations (type.h): a name and the function a guest call is continued with. */
typedef struct GLTANG_Continuation GLTANG_Continuation;

/**
 * @brief What a native function or a factory is given: its arguments, the
 *   handles it makes, and where it puts its answer. Opaque; valid for the
 *   duration of the call.
 */
typedef struct GLTANG_NativeCall GLTANG_NativeCall;

/**
 * @brief A native function.
 *
 * Read the arguments with the `gltang_call_*` accessors or the handle readers,
 * set the answer with one of the `gltang_call_return_*` calls, and return true.
 * Returning false, with no answer set, makes the call's value the error
 * `Host function failed`; returning with no answer set makes it null. An answer
 * set by ::gltang_call_return_error is the call's value whatever is returned.
 *
 * @param call The call.
 * @param user The pointer given at registration.
 * @return Whether the function worked.
 */
typedef bool (*GLTANG_NativeFn)(GLTANG_NativeCall * call, void * user);

/**
 * @brief A handle to a value, valid until the callback that made it returns.
 *
 * A zero handle names nothing and is refused wherever a value is wanted.
 */
typedef struct GLTANG_Handle {
  uint32_t scope;   ///< Which scope; never reused within an execution.
  uint32_t index;   ///< Which slot of it.
} GLTANG_Handle;

/** @brief A handle that outlives its callback: see ::gltang_handle_promote. */
typedef struct GLTANG_Persistent {
  uint32_t index;       ///< Which slot of the execution's table.
  uint32_t generation;  ///< Which use of the slot; a released handle is a tombstone.
} GLTANG_Persistent;

/** @brief One run of a string that carries one encoding tag. */
typedef struct GLTANG_StringSegment {
  size_t size;                 ///< `sizeof(GLTANG_StringSegment)`, set by the caller.
  size_t offset;               ///< Where the run starts, in bytes.
  size_t length;               ///< How long it is, in bytes.
  GLTANG_String_Type encoding; ///< The tag of every grapheme in it.
} GLTANG_StringSegment;

/**
 * @name Arguments, the bound value and the answer
 * @{
 */

/**
 * @brief A handle to argument `index`.
 *
 * @no_gc It makes a handle, which can be refused for the limit.
 * @param out Receives the handle. Written only on success.
 * @return ::GLTANG_OK; ::GLTANG_ERR_INVALID for a NULL argument or an index out
 *   of range; ::GLTANG_ERR_LIMIT when the scope is full or memory is out.
 */
GLTANG_API GLTANG_Result gltang_call_arg(
    GLTANG_NativeCall * call, size_t index, GLTANG_Handle * out);

/**
 * @brief The host context of the execution that is running this call: the
 *   pointer given to ::gltang_execution_set_host_context, or NULL.
 *
 * It is the per-request state a host hangs on an execution, so one native or
 * factory serves every execution and finds its own request here. The engine
 * never reads, copies or frees it.
 *
 * @no_gc
 */
GLTANG_API void * gltang_call_host_context(const GLTANG_NativeCall * call);

/**
 * @brief A handle to the value this native is bound to (`null` for one that is
 *   bound to nothing).
 *
 * @no_gc
 */
GLTANG_API GLTANG_Result gltang_call_bound(
    GLTANG_NativeCall * call, GLTANG_Handle * out);

/**
 * @brief Answers with the value a handle names, replacing any earlier answer.
 *
 * A handle that is not valid answers with the error `Host function failed`.
 *
 * @no_gc
 */
GLTANG_API void gltang_call_return_handle(
    GLTANG_NativeCall * call, GLTANG_Handle handle);

/**
 * @brief Polls the runtime from inside a callback, charging `work` units of
 *   fuel (AD-21).
 *
 * A callback whose work the program controls (a long loop over a collection
 * the program built) calls this every so often, so that a budget or a
 * request from the host is honoured while it runs. The poll allows only two
 * verdicts: go on, or stop the run. A stop makes every later call of the
 * callback fail and the run unwinds when the callback returns, whatever it
 * answers. A pause the host asks for is taken at the program's next poll.
 *
 * @may_gc
 * @param work Units of fuel the callback has done since it last polled; at
 *   least one is charged.
 * @return ::GLTANG_OK to go on; ::GLTANG_ERR_LIMIT when the run is to stop
 *   (return at once); ::GLTANG_ERR_INVALID for NULL.
 */
GLTANG_API GLTANG_Result gltang_call_poll(GLTANG_NativeCall * call, uint64_t work);

/** @brief The most arguments one guest call requested with ::gltang_call_guest passes. */
#define GLTANG_GUEST_CALL_ARGS_MAX 16u

/**
 * @brief Asks the engine to call guest code and then to call a continuation
 *   with the result (the resumable protocol, AD-23).
 *
 * Only a callback that is resumable may ask: an operation of a type that lists
 * it in `resumable_ops` (type.h), a native marked with
 * ::gltang_library_mark_resumable, or a continuation. The callback must
 * return ::GLTANG_OP_PENDING (an operation) or `true` (a native) at once and
 * touch nothing but its return: the engine ends the callback there, so no
 * handle, scope, text pointer or bytes pointer survives it, and the
 * continuation is given new ones. What the continuation needs is put in
 * `state`, a value of any kind (a slot of an instance, an array of its
 * own...).
 *
 * The callee is anything a `CALL` accepts, called exactly as a `CALL` calls
 * it: a function, a native, a template, a callable instance (which may itself
 * be pending). One that cannot be entered is not a failure of the request: the
 * error value is its result. A limit that unwinds the run ends it without a
 * continuation.
 *
 * @may_gc
 * @param call The call.
 * @param callee A handle of this call.
 * @param args Handles of this call, `argc` of them.
 * @param argc At most ::GLTANG_GUEST_CALL_ARGS_MAX.
 * @param continuation An index into the continuation table of the type (or of
 *   the native's member).
 * @param state A handle of this call, or a null handle (all zero) for none.
 * @return ::GLTANG_OK; ::GLTANG_ERR_INVALID when the callback may not ask, has
 *   asked already, `continuation` is outside the table, a handle is stale or
 *   another scope's, the callee is not a value a `CALL` accepts, or `argc` is
 *   too large. A refusal fails the whole call with `Host
 *   function failed`, and nothing the callback does after it (an answer, a
 *   decline, any status) changes that; a request made before it does not run.
 */
GLTANG_API GLTANG_Result gltang_call_guest(GLTANG_NativeCall * call,
    GLTANG_Handle callee, const GLTANG_Handle * args, size_t argc,
    uint32_t continuation, GLTANG_Handle state);

/**
 * @brief The dispatch step this callback continues: the zero-based index, in
 *   the order the engine calls them, of the callback within one execution of
 *   one instruction (`!=` asks `not_equal` of the left, then of the right at
 *   steps 0 and 1, then `equal` at steps 2 and 3; `print` asks `print` at 0
 *   and `to_string` at 1; the other operations ask at step 0, and a binary
 *   operator's reflected operand at 1). It is 0 on a first entry and in a
 *   native, and in a continuation it is the step that made the request.
 *
 * @no_gc
 */
GLTANG_API uint32_t gltang_call_resume_step(const GLTANG_NativeCall * call);

/** @} */

/**
 * @brief Opens a frame inside the callback's scope: the handles made until the
 *   matching ::gltang_handle_pop_frame belong to it and end with it.
 *
 * A callback that builds something large makes many handles it needs only
 * briefly (the pieces of one row of a table); a frame per row keeps the count
 * low. Handles of the outer frames stay valid while an inner frame is open, so a
 * value made outside can be filled from inside. The scope's limit counts the
 * handles of all the frames that are open.
 *
 * @no_gc
 * @return ::GLTANG_OK; ::GLTANG_ERR_LIMIT when memory is out.
 */
GLTANG_API GLTANG_Result gltang_handle_push_frame(GLTANG_NativeCall * call);

/**
 * @brief Closes the innermost frame this callback opened; its handles are gone.
 *
 * @no_gc
 * @return ::GLTANG_OK; ::GLTANG_ERR_INVALID when the callback has no frame of its
 *   own open.
 */
GLTANG_API GLTANG_Result gltang_handle_pop_frame(GLTANG_NativeCall * call);

/**
 * @name Readers
 *
 * Each takes the call that owns the handle. All are `@no_gc`.
 * @{
 */

/** @brief Whether the handle is valid in this callback. @no_gc */
GLTANG_API bool gltang_handle_valid(
    const GLTANG_NativeCall * call, GLTANG_Handle handle);

/** @brief The kind of value; ::GLTANG_KIND_NULL for a handle that is not valid. @no_gc */
GLTANG_API GLTANG_ValueKind gltang_handle_kind(
    const GLTANG_NativeCall * call, GLTANG_Handle handle);

/** @brief A boolean; false unless the handle names a true boolean. @no_gc */
GLTANG_API bool gltang_handle_bool(
    const GLTANG_NativeCall * call, GLTANG_Handle handle);

/** @brief An integer; 0 unless the handle names an integer. @no_gc */
GLTANG_API int64_t gltang_handle_integer(
    const GLTANG_NativeCall * call, GLTANG_Handle handle);

/** @brief A float; 0 unless the handle names a float. @no_gc */
GLTANG_API double gltang_handle_float(
    const GLTANG_NativeCall * call, GLTANG_Handle handle);

/**
 * @brief A string's bytes, every segment unencoded.
 *
 * The pointer is valid until the next call that can collect (one the header
 * labels may_gc) or the end of the callback.
 *
 * @no_gc
 * @param out_length Receives the byte count; may be NULL.
 * @return The NUL-terminated bytes; NULL unless the handle names a string.
 */
GLTANG_API const char * gltang_handle_text(const GLTANG_NativeCall * call,
    GLTANG_Handle handle, size_t * out_length);

/**
 * @brief Copies a string's bytes out, as `snprintf` does.
 *
 * @no_gc
 * @param buffer Receives up to `capacity - 1` bytes and a NUL; may be NULL with
 *   a capacity of 0.
 * @return The length the whole text needs, not counting the NUL; 0 unless the
 *   handle names a string.
 */
GLTANG_API size_t gltang_handle_copy_text(const GLTANG_NativeCall * call,
    GLTANG_Handle handle, char * buffer, size_t capacity);

/** @brief How many graphemes a string has; 0 unless the handle names a string. @no_gc */
GLTANG_API size_t gltang_handle_string_graphemes(
    const GLTANG_NativeCall * call, GLTANG_Handle handle);

/** @brief How many tagged runs a string has; 0 unless the handle names a string. @no_gc */
GLTANG_API size_t gltang_handle_string_segments(
    const GLTANG_NativeCall * call, GLTANG_Handle handle);

/**
 * @brief Run `index` of a string: where it is and the encoding tag it carries.
 *
 * @no_gc
 * @param out Receives the run; its `size` is set by the caller.
 * @return Whether there is such a run.
 */
GLTANG_API bool gltang_handle_string_segment(const GLTANG_NativeCall * call,
    GLTANG_Handle handle, size_t index, GLTANG_StringSegment * out);

/** @brief An array's length; 0 unless the handle names an array. @no_gc */
GLTANG_API size_t gltang_handle_array_length(
    const GLTANG_NativeCall * call, GLTANG_Handle handle);

/**
 * @brief A handle to element `index` of an array. It names the element itself,
 *   not a copy.
 *
 * @no_gc
 * @return ::GLTANG_OK; ::GLTANG_ERR_INVALID for a handle that is not an array
 *   or an index out of range; ::GLTANG_ERR_LIMIT for a full scope.
 */
GLTANG_API GLTANG_Result gltang_handle_array_get(GLTANG_NativeCall * call,
    GLTANG_Handle array, size_t index, GLTANG_Handle * out);

/** @brief How many members a map has; 0 unless the handle names a map. @no_gc */
GLTANG_API size_t gltang_handle_map_count(
    const GLTANG_NativeCall * call, GLTANG_Handle handle);

/**
 * @brief A handle to the value under a key.
 *
 * @no_gc
 * @param found Receives whether the map holds the key; may be NULL.
 * @return ::GLTANG_OK (with `*found` false and `out` untouched for an absent
 *   key); ::GLTANG_ERR_INVALID for a handle that is not a map; ::GLTANG_ERR_LIMIT.
 */
GLTANG_API GLTANG_Result gltang_handle_map_get(GLTANG_NativeCall * call,
    GLTANG_Handle map, const char * key, size_t key_length, bool * found,
    GLTANG_Handle * out);

/**
 * @brief A handle to the key of member `index`, in the order the members were
 *   set. The key is a string and keeps its encoding tag.
 *
 * @no_gc
 */
GLTANG_API GLTANG_Result gltang_handle_map_key_at(GLTANG_NativeCall * call,
    GLTANG_Handle map, size_t index, GLTANG_Handle * out);

/** @brief A handle to the value of member `index`. @no_gc */
GLTANG_API GLTANG_Result gltang_handle_map_value_at(GLTANG_NativeCall * call,
    GLTANG_Handle map, size_t index, GLTANG_Handle * out);

/** @} */

/**
 * @name Constructors
 *
 * Each makes a value in the program's heap and gives a handle to it. All are
 * `@may_gc` except the two that allocate nothing, ::gltang_handle_make_null and
 * ::gltang_handle_make_bool. Each returns ::GLTANG_OK, ::GLTANG_ERR_INVALID for a NULL argument,
 * a handle that is not valid or of the wrong kind, or text that is not UTF-8,
 * and ::GLTANG_ERR_LIMIT for a full scope, the context's memory, or a budget
 * that orders the run to stop (the callback's value is then `Out of memory`, or
 * the run unwinds).
 * @{
 */

/** @brief Makes `null`. @no_gc */
GLTANG_API GLTANG_Result gltang_handle_make_null(
    GLTANG_NativeCall * call, GLTANG_Handle * out);

/** @brief Makes a boolean. @no_gc */
GLTANG_API GLTANG_Result gltang_handle_make_bool(
    GLTANG_NativeCall * call, bool value, GLTANG_Handle * out);

/** @brief Makes an integer. @may_gc */
GLTANG_API GLTANG_Result gltang_handle_make_integer(
    GLTANG_NativeCall * call, int64_t value, GLTANG_Handle * out);

/** @brief Makes a float. @may_gc */
GLTANG_API GLTANG_Result gltang_handle_make_float(
    GLTANG_NativeCall * call, double value, GLTANG_Handle * out);

/**
 * @brief Makes a string with one encoding tag.
 *
 * @may_gc
 * @param text UTF-8, `length` bytes.
 */
GLTANG_API GLTANG_Result gltang_handle_make_string(GLTANG_NativeCall * call,
    const char * text, size_t length, GLTANG_String_Type encoding,
    GLTANG_Handle * out);

/**
 * @brief Makes a string from two strings, each keeping its tags (the language's
 *   `a + b`).
 *
 * @may_gc
 */
GLTANG_API GLTANG_Result gltang_handle_make_concat(GLTANG_NativeCall * call,
    GLTANG_Handle first, GLTANG_Handle second, GLTANG_Handle * out);

/**
 * @brief Makes an empty array.
 *
 * @may_gc
 * @param capacity How many elements to make room for; a hint.
 */
GLTANG_API GLTANG_Result gltang_handle_make_array(
    GLTANG_NativeCall * call, size_t capacity, GLTANG_Handle * out);

/** @brief Appends a copy of `element` to `array`. @may_gc */
GLTANG_API GLTANG_Result gltang_handle_array_push(GLTANG_NativeCall * call,
    GLTANG_Handle array, GLTANG_Handle element);

/** @brief Makes an empty map. @may_gc */
GLTANG_API GLTANG_Result gltang_handle_make_map(
    GLTANG_NativeCall * call, size_t capacity, GLTANG_Handle * out);

/**
 * @brief Sets `key` in `map` to a copy of `value`; a key set twice keeps its
 *   first position and takes the last value.
 *
 * @may_gc
 * @param key UTF-8, `key_length` bytes.
 * @param key_encoding The tag the key carries.
 */
GLTANG_API GLTANG_Result gltang_handle_map_set(GLTANG_NativeCall * call,
    GLTANG_Handle map, const char * key, size_t key_length,
    GLTANG_String_Type key_encoding, GLTANG_Handle value);

/**
 * @brief Makes a library into a value: `use` and `.` read its members.
 *
 * The library is sealed and the execution retains it until the execution is
 * destroyed, so the host may release its own reference at once.
 *
 * @may_gc
 */
GLTANG_API GLTANG_Result gltang_handle_make_library(GLTANG_NativeCall * call,
    GLTANG_Library * library, GLTANG_Handle * out);

/**
 * @brief Makes a native function, bound to a value, that the program can call
 *   (`obj.method()` where `method` is this).
 *
 * The function reads the value it is bound to with ::gltang_call_bound. The
 * execution owns the function's record; `user` is the host's, and must outlive
 * every call. A snapshot of an execution holding one is refused.
 *
 * @may_gc
 * @param bound A handle to the value to bind (copied if it is a container), or
 *   a zero handle for `null`.
 */
GLTANG_API GLTANG_Result gltang_handle_make_native(GLTANG_NativeCall * call,
    GLTANG_NativeFn function, void * user, GLTANG_Handle bound,
    GLTANG_Handle * out);

/**
 * @brief ::gltang_handle_make_native for a *resumable* native: one that may ask
 *   the engine to call guest code (::gltang_call_guest) and is called again
 *   through an entry of `continuations` (a table the execution copies).
 *
 * This is how an attribute function of a built-in kind answers a method that
 * calls guest code (`s.each(fn)`): the attribute function itself is never
 * resumable, but the native it answers may be. The continuation is given the
 * `user` of this call, and reads the bound value with ::gltang_call_bound.
 * The native is called as a library native marked with
 * ::gltang_library_mark_resumable is, and a snapshot of an execution holding
 * one is refused.
 *
 * @may_gc
 * @return ::GLTANG_OK; ::GLTANG_ERR_INVALID for a NULL argument or a table
 *   ::gltang_library_mark_resumable would refuse.
 */
GLTANG_API GLTANG_Result gltang_handle_make_resumable_native(GLTANG_NativeCall * call,
    GLTANG_NativeFn function, void * user, const GLTANG_Continuation * continuations,
    size_t continuation_count, GLTANG_Handle bound, GLTANG_Handle * out);

/** @} */

/**
 * @name Custom values (type.h)
 *
 * An instance of a host-defined type: its slots, which hold values the
 * collector sees, and its raw bytes, which are copied in and out.
 * @{
 */

/**
 * @brief Makes an instance of `type`, every slot null and every byte zero.
 *
 * @may_gc
 * @param type A type of a registry attached to this execution
 *   (::gltang_execution_attach_types, type.h).
 * @return ::GLTANG_OK; ::GLTANG_ERR_INVALID for a NULL argument or a type
 *   whose registry this execution has not attached; ::GLTANG_ERR_LIMIT as for
 *   every constructor.
 */
GLTANG_API GLTANG_Result gltang_handle_make_custom(GLTANG_NativeCall * call,
    const GLTANG_Type * type, GLTANG_Handle * out);

/**
 * @brief The type of a custom value; NULL unless the handle names one.
 *
 * @no_gc
 */
GLTANG_API const GLTANG_Type * gltang_handle_custom_type(
    const GLTANG_NativeCall * call, GLTANG_Handle handle);

/**
 * @brief A handle to what slot `index` of a custom value holds. It names the
 *   value itself, not a copy.
 *
 * @no_gc
 * @return ::GLTANG_OK; ::GLTANG_ERR_INVALID for a handle that is not a custom
 *   value or an index past its slots; ::GLTANG_ERR_LIMIT for a full scope.
 */
GLTANG_API GLTANG_Result gltang_handle_custom_slot_get(GLTANG_NativeCall * call,
    GLTANG_Handle handle, size_t index, GLTANG_Handle * out);

/**
 * @brief Stores `value` in slot `index` of a custom value; a container is
 *   copied, as every store of one into another value is.
 *
 * @may_gc
 * @return ::GLTANG_OK; ::GLTANG_ERR_INVALID for a handle that is not valid or
 *   not a custom value, or an index past its slots; ::GLTANG_ERR_LIMIT as for
 *   every constructor.
 */
GLTANG_API GLTANG_Result gltang_handle_custom_slot_set(GLTANG_NativeCall * call,
    GLTANG_Handle handle, size_t index, GLTANG_Handle value);

/**
 * @brief Copies `length` raw bytes from `offset` of a custom value out.
 *
 * @no_gc
 * @return ::GLTANG_OK; ::GLTANG_ERR_INVALID for a handle that is not a custom
 *   value, a NULL buffer with a length, or a range past its bytes (then
 *   nothing is copied).
 */
GLTANG_API GLTANG_Result gltang_handle_custom_read_bytes(
    const GLTANG_NativeCall * call, GLTANG_Handle handle, size_t offset,
    void * buffer, size_t length);

/**
 * @brief Copies `length` bytes into a custom value's raw bytes at `offset`.
 *
 * @no_gc
 * @return As ::gltang_handle_custom_read_bytes (nothing is written on a
 *   refusal).
 */
GLTANG_API GLTANG_Result gltang_handle_custom_write_bytes(
    GLTANG_NativeCall * call, GLTANG_Handle handle, size_t offset,
    const void * bytes, size_t length);

/**
 * @brief Marks a custom value as standing for an error, or clears the mark.
 *
 * The mark is per instance, in the object's header, and read by a pure test.
 * A marked instance is falsy (its type's `truthy` is not asked), and it
 * contaminates where an engine error value does: as an operand of an
 * operator, as a function's result, as a statement's value. It is not an
 * engine error: it is never listed in the error list, fires no error-created
 * hook and has no error kind.
 *
 * @no_gc
 * @return ::GLTANG_OK; ::GLTANG_ERR_INVALID for a handle that is not a custom
 *   value.
 */
GLTANG_API GLTANG_Result gltang_handle_custom_set_error(
    GLTANG_NativeCall * call, GLTANG_Handle handle, bool is_error);

/**
 * @brief Whether a custom value is marked as standing for an error
 *   (::gltang_handle_custom_set_error). False for any other handle.
 *
 * @no_gc
 */
GLTANG_API bool gltang_handle_custom_is_error(
    const GLTANG_NativeCall * call, GLTANG_Handle handle);

/** @} */

/**
 * @name Persistent handles
 * @{
 */

/**
 * @brief Promotes a handle to a persistent one.
 *
 * @no_gc It makes a table slot.
 * @return ::GLTANG_OK; ::GLTANG_ERR_INVALID; ::GLTANG_ERR_LIMIT for a full table
 *   or the context's memory.
 */
GLTANG_API GLTANG_Result gltang_handle_promote(GLTANG_NativeCall * call,
    GLTANG_Handle handle, GLTANG_Persistent * out);

/**
 * @brief A handle, in this callback's scope, to a persistent value.
 *
 * @no_gc
 * @return ::GLTANG_OK; ::GLTANG_ERR_INVALID for a released or foreign persistent.
 */
GLTANG_API GLTANG_Result gltang_persistent_get(GLTANG_NativeCall * call,
    GLTANG_Persistent persistent, GLTANG_Handle * out);

/**
 * @brief Releases a persistent handle; the value may then be collected.
 *
 * @no_gc
 * @return ::GLTANG_OK; ::GLTANG_ERR_INVALID if it is already released.
 */
GLTANG_API GLTANG_Result gltang_persistent_release(
    GLTANG_NativeCall * call, GLTANG_Persistent persistent);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GLTANG_HANDLE_H */
