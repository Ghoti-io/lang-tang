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
 * @file library.h
 * @stability free
 *
 * Libraries: what a host gives a Tang program (language reference, section 9).
 *
 * A ::GLTANG_Library is a named table of members. A host builds one with the
 * `gltang_library_add_*` calls (null, boolean, integer, float and string
 * values, described values (arrays and maps: description.h), native functions,
 * compiled templates, other libraries, and lazy factories), then attaches it to an execution (::gltang_execution_set_libraries)
 * or to a program (::gltang_program_set_libraries). The program reads it with
 * `use name;`, which binds the member, and `use name.member as m;`, which binds
 * a member of a member.
 *
 * **Sealing.** A library is mutable only until it is attached anywhere: to an
 * execution, to a program, or as a member of another library. From then on
 * every `gltang_library_add_*` call refuses with ::GLTANG_ERR_INVALID. That is
 * what lets one library be read by many contexts on many threads with no lock,
 * what keeps a program immutable (AD-22), and what makes a cycle of libraries
 * impossible to build: a library that holds another has sealed it first.
 *
 * **Threads.** Building a library (the `gltang_library_add_*` calls) is
 * single-threaded: one thread builds it, then attaches it. After that it is
 * read-only, and its native functions and factories may run concurrently from
 * many contexts on many threads, so they must be safe for that (their `user`
 * pointers are shared).
 *
 * **Reference counting.** A library is reference counted, atomically. The
 * creator's reference is the one ::gltang_library_create returns; whatever a
 * library is attached to takes its own.
 *
 * **Resolution** (language reference 9.1). The first name of a `use` path is
 * looked up in the execution's library, then the program's, then the built-ins
 * (`math` and `random`); the first library that has the name wins, so a host
 * can shadow `math`. The rest of the path is read with the language's
 * attribute rule: a member of a library, `Not implemented` for an absent one. A
 * first name nothing provides binds null.
 *
 * **Host functions are opaque (AD-23), unless marked resumable.** A native
 * function or a factory runs synchronously, on the thread that owns the context,
 * inside the run. It is given a call object (::GLTANG_NativeCall) and its `user`
 * pointer, and nothing else: it cannot pause, and it cannot call back into guest
 * code, except a native marked with ::gltang_library_mark_resumable, which asks
 * the engine to call guest code and to call it again with the result
 * (::gltang_call_guest). Anything it
 * calls on the execution that is running it (any `gltang_execution_*` setter,
 * ::grcore_run, ::grcore_resume) is refused: the setters with
 * ::GLTANG_ERR_INVALID, the runtime calls with ::GRCORE_ERR_INVALID, and
 * ::gltang_execution_destroy does nothing. A native
 * costs fuel like any call, plus one unit per ::GLTANG_WORK_BYTES_PER_FUEL
 * bytes it returns. Each call runs under an activation record of its own, which
 * draws one unit on the context's native-depth budget
 * (`grcore_options_set_native_depth`) for as long as it runs: a call the budget
 * refuses is not made and is the recursion-limit error, whether the program is
 * interpreted or compiled. The arguments the call object reads are a copy.
 *
 * **Templates.** A template member holds a compiled program (the library
 * retains it) with its own budget scope. `use sidebar;` binds a function value
 * and `sidebar()` runs the program as a guest-to-guest call: see
 * ::gltang_library_add_template.
 *
 * This header is `free`: it names ::GLTANG_Program and ::GLTANG_ErrorKind, which
 * are, and a template member cannot be described without them.
 */

#ifndef GHOTI_IO_GLTANG_LIBRARY_H
#define GHOTI_IO_GLTANG_LIBRARY_H

#include <ghoti.io/lang-tang/macros.h>

#include <ghoti.io/lang-tang/core.h>
#include <ghoti.io/lang-tang/description.h>
#include <ghoti.io/lang-tang/handle.h>
#include <ghoti.io/lang-tang/program.h>
#include <ghoti.io/lang-tang/type.h>
#include <ghoti.io/lang-tang/unicodeString.h>
#include <ghoti.io/lang-tang/value.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GLTANG_Library, GLTANG_NativeCall and GLTANG_NativeFn are declared in handle.h (stable), which this header includes. */

/**
 * @brief A lazy factory: makes a scalar when a `use` that reaches it executes.
 *
 * Never called at registration. Fills `out` with a null, boolean, integer,
 * float or string; the engine copies it before this returns. Returns false for
 * "not available", which binds null.
 *
 * @param user The pointer given at registration.
 * @param out The value to fill.
 * @return Whether there is a value.
 */
typedef bool (*GLTANG_FactoryFn)(void * user, GLTANG_HostValue * out);

/**
 * @brief A lazy value factory: fills a description when a `use` that reaches it
 *   executes.
 *
 * Never called at registration. `out` is an empty description that belongs to
 * the run (its memory is the context's, and it is freed when the factory
 * returns): build the value in it with the `gltang_value_*` calls and make it the
 * root, then return true. Returning false is "not available", which binds null;
 * a description left without a root is null. Called once for each `use` that
 * reaches it, so each reach gets a fresh copy.
 *
 * @param user The pointer given at registration.
 * @param out The description to fill.
 * @return Whether there is a value.
 */
typedef bool (*GLTANG_ValueFactoryFn)(void * user, GLTANG_ValueDescription * out);

/** @brief What a template call does when its own budget runs out (AD-21). */
typedef enum {
  GLTANG_SCOPE_EMPTY = 0, ///< Unwind to the call; its value is the limit error.
  GLTANG_SCOPE_SEGMENTS,  ///< Unwind to the call; its value is the segments completed so far.
  GLTANG_SCOPE_PAUSE      ///< Pause so the host can raise the scope's budget (a development setting).
} GLTANG_ScopePolicy;

/**
 * @brief Makes an empty library.
 *
 * @param name The library's name: its key when it is added to another library,
 *   and what a program that prints it sees (`Library: name`). Copied. NULL makes
 *   the unnamed library an execution or a program takes as its registry root.
 * @param out_library Receives the library, with one reference. Written only on
 *   success.
 * @return ::GLTANG_OK; ::GLTANG_ERR_INVALID for NULL `out_library`, or a name
 *   that is empty or holds a dot; ::GLTANG_ERR_OOM.
 */
GLTANG_API GLTANG_Result gltang_library_create(
    const char * name, GLTANG_Library ** out_library);

/**
 * @brief Takes another reference.
 *
 * @param library The library, or NULL.
 * @return `library`.
 */
GLTANG_API GLTANG_Library * gltang_library_retain(GLTANG_Library * library);

/**
 * @brief Gives a reference back; the last one frees the library.
 *
 * @param library The library, or NULL (ignored).
 */
GLTANG_API void gltang_library_release(GLTANG_Library * library);

/**
 * @brief Whether the library has been attached, so that it can no longer change.
 *
 * @param library The library.
 * @return True when sealed; false for NULL.
 */
GLTANG_API bool gltang_library_sealed(const GLTANG_Library * library);

/**
 * @brief The library's name.
 *
 * @param library The library.
 * @return The name, owned by the library; NULL for the unnamed library or NULL.
 */
GLTANG_API const char * gltang_library_name(const GLTANG_Library * library);

/**
 * @brief How many members the library has.
 *
 * @param library The library.
 * @return The count; 0 for NULL.
 */
GLTANG_API size_t gltang_library_count(const GLTANG_Library * library);

/**
 * @name Adding members
 *
 * Each call adds one member under `name` and refuses, changing nothing, with
 * ::GLTANG_ERR_INVALID when the library is NULL or sealed, the name is empty,
 * holds a dot or is already a member, or an argument is NULL; with
 * ::GLTANG_ERR_OOM when memory runs out.
 * @{
 */

/** @brief Adds `null`. */
GLTANG_API GLTANG_Result gltang_library_add_null(
    GLTANG_Library * library, const char * name);

/** @brief Adds a boolean. */
GLTANG_API GLTANG_Result gltang_library_add_bool(
    GLTANG_Library * library, const char * name, bool value);

/** @brief Adds an integer. */
GLTANG_API GLTANG_Result gltang_library_add_integer(
    GLTANG_Library * library, const char * name, int64_t value);

/** @brief Adds a float. */
GLTANG_API GLTANG_Result gltang_library_add_float(
    GLTANG_Library * library, const char * name, double value);

/**
 * @brief Adds a string, copied.
 *
 * @param text UTF-8, `length` bytes; need not be NUL-terminated. A text that is
 *   not valid UTF-8 binds null when a program reaches it.
 * @param length The byte count.
 * @param encoding The segment tag the string carries when the program prints
 *   it (::GLTANG_UNICODE_STRING_TYPE_HTML makes it escaped on output).
 */
GLTANG_API GLTANG_Result gltang_library_add_string(GLTANG_Library * library,
    const char * name, const char * text, size_t length,
    GLTANG_String_Type encoding);

/**
 * @brief Adds a native function.
 *
 * @param function The function.
 * @param user Handed to it on every call (its bound state).
 */
GLTANG_API GLTANG_Result gltang_library_add_native(GLTANG_Library * library,
    const char * name, GLTANG_NativeFn function, void * user);

/**
 * @brief Marks a native member resumable: it may ask the engine to call guest
 *   code with ::gltang_call_guest, and is then called back through one entry of
 *   `continuations` (a table the library copies).
 *
 * A resumable native requests by returning `true` after ::gltang_call_guest;
 * its continuation is a ::GLTANG_ContinuationFn, given the member's `user`, and
 * answers with ::GLTANG_OP_OK (the value set through the call object) or
 * ::GLTANG_OP_ERROR, or chains with ::GLTANG_OP_PENDING. Compiled code does not
 * call a resumable native: the call is an exit to the interpreter. An attribute
 * function of a built-in kind and a factory are never resumable (answer a
 * resumable native bound with ::gltang_handle_make_resumable_native
 * instead).
 *
 * @param library An unsealed library.
 * @param name A native member already added.
 * @param continuations The table; entries as in ::GLTANG_TypeDescriptor.
 * @param continuation_count 1 to ::GLTANG_CONTINUATIONS_MAX.
 * @return ::GLTANG_OK; ::GLTANG_ERR_INVALID for a NULL argument, a sealed
 *   library, a member that is not a native, one marked already, an empty or
 *   over-long table, or a bad, repeated or function-less entry; ::GLTANG_ERR_OOM.
 */
GLTANG_API GLTANG_Result gltang_library_mark_resumable(GLTANG_Library * library,
    const char * name, const GLTANG_Continuation * continuations,
    size_t continuation_count);

/**
 * @brief Adds a lazy factory that answers with any value, called each time a
 *   `use` that reaches it executes and never at registration.
 *
 * The function is given a call object with no arguments, makes what it answers
 * with handles (handle.h) and answers with ::gltang_call_return_handle or any
 * other `gltang_call_return_*`, so it can answer with a container, a native
 * function bound to a value, or a **library** (`use pages.recent as r;` where
 * `recent` is made by a factory). Returning false is "not available", which
 * binds null.
 *
 * @param factory The factory; see ::GLTANG_NativeFn.
 * @param user Handed to it.
 */
GLTANG_API GLTANG_Result gltang_library_add_handle_factory(
    GLTANG_Library * library, const char * name, GLTANG_NativeFn factory,
    void * user);

/**
 * @brief Adds a lazy factory, called each time a `use` that reaches it
 *   executes and never at registration.
 *
 * @param factory The factory.
 * @param user Handed to it.
 */
GLTANG_API GLTANG_Result gltang_library_add_factory(GLTANG_Library * library,
    const char * name, GLTANG_FactoryFn factory, void * user);

/**
 * @brief Adds a described value (description.h): an array, a map, or any value a
 *   host built.
 *
 * `use name;` binds a fresh copy of it in the execution's heap each time the
 * `use` executes: the copies are independent of each other and of the
 * description. The library retains the description and seals it.
 *
 * @param description The description, with its root set.
 */
GLTANG_API GLTANG_Result gltang_library_add_value(GLTANG_Library * library,
    const char * name, GLTANG_ValueDescription * description);

/**
 * @brief Adds a lazy described value, built when a `use` that reaches it
 *   executes and never at registration.
 *
 * @param factory The factory.
 * @param user Handed to it.
 */
GLTANG_API GLTANG_Result gltang_library_add_value_factory(
    GLTANG_Library * library, const char * name, GLTANG_ValueFactoryFn factory,
    void * user);

/**
 * @brief Adds a compiled template.
 *
 * `use name;` binds a function value; calling it, `name()`, takes no arguments
 * (any argument is `Argument Count Mismatch`) and runs the program as a
 * guest-to-guest call on the caller's own stack. The call has its own program
 * scope (variables fresh for each call) and its own output: the call's value is
 * that output as a string, every segment's encoding tag kept, so
 * `print(name())` appends it with each piece escaped as it says. A template gets
 * its data from the libraries the execution carries, which it shares with the
 * caller.
 *
 * Each call opens a budget scope (AD-21) with `scope_fuel` as its own exclusive
 * budget, under the context's inclusive fuel budget. When the scope runs out
 * the policy decides: ::GLTANG_SCOPE_EMPTY stops only this call and its value
 * is the error `Limit Exceeded`; ::GLTANG_SCOPE_SEGMENTS the same, but its
 * value is the segments completed so far; ::GLTANG_SCOPE_PAUSE pauses the run
 * at the callee's file and line so the host can raise the scope with
 * ::grcore_context_fuel_scope_set_budget (on ::grcore_context_fuel_scope_top)
 * and resume, or unwind. The EMPTY and SEGMENTS outcomes are entered in the
 * execution's error list as a scope limit; PAUSE records none (a pause is not
 * a stop yet), and a PAUSE scope that runs out inside a native operation (which
 * cannot pause) unwinds the whole run with the limit instead.
 *
 * @param program The compiled template. The library retains it.
 * @param scope_fuel The scope's own budget (exclusive), or ::GRCORE_UNLIMITED
 *   (which is still under the context's fuel budget).
 * @param policy What happens when it runs out.
 */
GLTANG_API GLTANG_Result gltang_library_add_template(GLTANG_Library * library,
    const char * name, GLTANG_Program * program, uint64_t scope_fuel,
    GLTANG_ScopePolicy policy);

/**
 * @brief Adds a library as a member, under its own name.
 *
 * The child is retained and sealed. A library cannot contain itself.
 *
 * @param child A library with a name.
 */
GLTANG_API GLTANG_Result gltang_library_add_library(
    GLTANG_Library * library, GLTANG_Library * child);

/** @} */

/**
 * @name The call object
 *
 * What a native function reads its arguments with and answers with. The text
 * returned by ::gltang_call_text is valid until the function returns.
 * @{
 */

/** @brief How many arguments the call has. */
GLTANG_API size_t gltang_call_count(const GLTANG_NativeCall * call);

/** @brief The kind of argument `index`; ::GLTANG_KIND_NULL out of range. */
GLTANG_API GLTANG_ValueKind gltang_call_kind(
    const GLTANG_NativeCall * call, size_t index);

/** @brief Argument `index` as a boolean; false unless it is a true boolean. */
GLTANG_API bool gltang_call_bool(const GLTANG_NativeCall * call, size_t index);

/** @brief Argument `index` as an integer; 0 unless it is an integer. */
GLTANG_API int64_t gltang_call_integer(
    const GLTANG_NativeCall * call, size_t index);

/** @brief Argument `index` as a float; 0 unless it is a float. */
GLTANG_API double gltang_call_float(
    const GLTANG_NativeCall * call, size_t index);

/**
 * @brief Argument `index` as text: a string's bytes, every segment unencoded.
 *
 * @param out_length Receives the byte count; may be NULL.
 * @return The NUL-terminated bytes; NULL unless it is a string.
 */
GLTANG_API const char * gltang_call_text(
    const GLTANG_NativeCall * call, size_t index, size_t * out_length);

/** @brief Answers with `null`. */
GLTANG_API void gltang_call_return_null(GLTANG_NativeCall * call);

/** @brief Answers with a boolean. */
GLTANG_API void gltang_call_return_bool(GLTANG_NativeCall * call, bool value);

/** @brief Answers with an integer. */
GLTANG_API void gltang_call_return_integer(
    GLTANG_NativeCall * call, int64_t value);

/** @brief Answers with a float. */
GLTANG_API void gltang_call_return_float(GLTANG_NativeCall * call, double value);

/**
 * @brief Answers with a string, copied now.
 *
 * @param text UTF-8, `length` bytes. Text that is not valid UTF-8 answers with
 *   the error `Host function failed`.
 * @param encoding The segment tag of the string.
 */
GLTANG_API void gltang_call_return_string(GLTANG_NativeCall * call,
    const char * text, size_t length, GLTANG_String_Type encoding);

/**
 * @brief Makes an empty description for this call to build a value in.
 *
 * Its memory is the context's own, so it is charged to the context, and it
 * belongs to the call: build it with the `gltang_value_*` calls, answer with it
 * (::gltang_call_return_value), and it is freed when the call returns. It cannot
 * be retained. One description for each call.
 *
 * @param out_description Receives the description. Written only on success.
 * @return ::GLTANG_OK; ::GLTANG_ERR_INVALID for a NULL argument or a second
 *   description in one call; ::GLTANG_ERR_OOM (the context's budget is out).
 */
GLTANG_API GLTANG_Result gltang_call_description(GLTANG_NativeCall * call,
    GLTANG_ValueDescription ** out_description);

/**
 * @brief Answers with the value a description describes, made fresh in the
 *   caller's heap.
 *
 * `description` is the call's own (::gltang_call_description), or a description
 * the host built earlier and keeps, which is sealed by being answered and
 * retained for the call; it may be answered again by any later call, from any
 * context. Answering with a description that cannot be made into a value
 * (memory, the budget, a limit) makes the call's value `Out of memory` or stops
 * the run, as any allocation does.
 */
GLTANG_API void gltang_call_return_value(GLTANG_NativeCall * call,
    GLTANG_ValueDescription * description);

/** @brief Answers with an error value of this kind. */
GLTANG_API void gltang_call_return_error(
    GLTANG_NativeCall * call, GLTANG_ErrorKind kind);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GLTANG_LIBRARY_H */
