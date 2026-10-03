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
 * @file execution.h
 * @stability free
 *
 * Running a program: an execution, its entry function, its result and its
 * output.
 *
 * A host makes the group, the options, the context and the heap with
 * runtime-core and runtime-heap, creates an execution here for that context
 * and program, and calls ::grcore_run with ::gltang_execution_entry itself.
 * Everything the interpreter needs to continue lives in the context's guest
 * stack (function, program counter, locals and operand stack in each frame),
 * so a pause saves nothing outside it: ::grcore_resume calls the same entry
 * again, on this thread or another, and the run carries on.
 *
 * What the host gives the program is in library.h (`math`, `random`, values,
 * native functions and templates, attached with ::gltang_execution_set_libraries
 * or ::gltang_program_set_libraries), and where its random numbers come from is
 * in seeds.h. The setters here are refused with ::GLTANG_ERR_INVALID once the
 * execution has started, and from inside a host function (a native function or
 * a factory): see library.h for that contract.
 *
 * Threads: an execution belongs to its context. Call it from the thread that
 * owns the context, and let it migrate with the context
 * (::grcore_context_release and ::grcore_context_acquire).
 */

#ifndef GHOTI_IO_GLTANG_EXECUTION_H
#define GHOTI_IO_GLTANG_EXECUTION_H

#include <ghoti.io/lang-tang/macros.h>

#include <ghoti.io/lang-tang/core.h>
#include <ghoti.io/lang-tang/library.h>
#include <ghoti.io/lang-tang/program.h>
#include <ghoti.io/lang-tang/seeds.h>
#include <ghoti.io/lang-tang/value.h>
#include <ghoti.io/runtime-core/b/context.h>
#include <ghoti.io/runtime-core/b/run.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A program running in a context. Opaque; owned by its context. */
typedef struct GLTANG_Execution GLTANG_Execution;

/** @brief Where an execution is. */
typedef enum {
  GLTANG_EXECUTION_NEW = 0,   ///< Not started.
  GLTANG_EXECUTION_RUNNING,   ///< Inside ::grcore_run or ::grcore_resume.
  GLTANG_EXECUTION_PAUSED,    ///< A poll paused it; ::grcore_resume continues.
  GLTANG_EXECUTION_FINISHED,  ///< The program ran to its end.
  GLTANG_EXECUTION_UNWOUND    ///< A poll unwound it; the frames are gone.
} GLTANG_ExecutionState;

/**
 * @brief Creates the execution of `program` in `context`.
 *
 * Registers the engine descriptor `lang-tang` with the context (which creates
 * the guest stack), the execution's root source, and the execution itself as
 * the context's keyed state, so a context has at most one. The heap must
 * already exist (::grheap_heap_create) and should have been configured with
 * ::gltang_heap_options_configure. The execution takes a reference to the
 * program. Everything it allocates is charged to the context.
 *
 * It lives as long as the context: destroying the context frees it.
 *
 * @param context The context, parked, owned by the caller, with a heap.
 * @param program The program.
 * @param out_execution Receives the execution. Written only on success.
 * @return ::GLTANG_OK; ::GLTANG_ERR_INVALID for a NULL argument, a context
 *   with no heap, or one that already has an execution; ::GLTANG_ERR_LIMIT when
 *   the context's memory budget refuses an allocation; ::GLTANG_ERR_OOM. A
 *   refusal leaves the context as it was.
 */
GLTANG_API GLTANG_Result gltang_execution_create(GRCORE_Context * context,
    GLTANG_Program * program, GLTANG_Execution ** out_execution);

/**
 * @brief The entry function to give ::grcore_run.
 *
 * `state` is the execution. Runs the program until it finishes, pauses at a
 * poll (function entry or a loop back-edge) or is unwound. Call it only
 * through ::grcore_run and ::grcore_resume.
 *
 * @param context The context ::grcore_run was given.
 * @param state The execution.
 * @return The step the run ended with.
 */
GLTANG_API GRCORE_Step gltang_execution_entry(
    GRCORE_Context * context, void * state);

/**
 * @brief Ends the execution early: releases the program, the output and the
 *   execution's roots.
 *
 * Optional: destroying the context does the same. After it every accessor
 * answers as for a program that never ran, and the context cannot take another
 * execution.
 *
 * @param execution The execution, or NULL (ignored). The context must not be
 *   running.
 */
GLTANG_API void gltang_execution_destroy(GLTANG_Execution * execution);

/**
 * @brief Attaches the execution's libraries: the first layer of every `use`
 *   (language reference, section 9.1), ahead of the program's and the built-ins.
 *
 * This is how a host injects a context: one compiled program, many executions,
 * each with its own `user` library. The library is sealed and retained; a
 * second call replaces the first.
 *
 * @param execution The execution.
 * @param library The library, or NULL for none.
 * @return ::GLTANG_OK; ::GLTANG_ERR_INVALID for a NULL execution, one that has
 *   started, or a call from inside a host function.
 */
GLTANG_API GLTANG_Result gltang_execution_set_libraries(
    GLTANG_Execution * execution, GLTANG_Library * library);

/**
 * @brief Sets the seed sequence the execution's generators are seeded from.
 *
 * `random.global` takes the next seed the first time it is read, and every read
 * of `random.default` takes one. Without a sequence, the execution makes a
 * private one from operating-system entropy when it first needs a seed. The
 * sequence is retained.
 *
 * @param execution The execution.
 * @param seeds The sequence, or NULL for none.
 * @return As ::gltang_execution_set_libraries.
 */
GLTANG_API GLTANG_Result gltang_execution_set_seeds(
    GLTANG_Execution * execution, GLTANG_SeedSequence * seeds);

/**
 * @brief Names the main program for the error list (the template an error
 *   belongs to when no template call is above it).
 *
 * The default is the program's file name, and `main` for a program with no file
 * name. Copied.
 *
 * @param execution The execution.
 * @param name The name.
 * @return As ::gltang_execution_set_libraries; ::GLTANG_ERR_OOM.
 */
GLTANG_API GLTANG_Result gltang_execution_set_name(
    GLTANG_Execution * execution, const char * name);

/**
 * @brief Where the execution is.
 *
 * @param execution The execution.
 * @return The state; ::GLTANG_EXECUTION_NEW for NULL.
 */
GLTANG_API GLTANG_ExecutionState gltang_execution_state(
    const GLTANG_Execution * execution);

/**
 * @brief The kind of the program's result: the value of the last statement
 *   executed (language reference, section 11).
 *
 * @param execution The execution.
 * @return The kind; ::GLTANG_KIND_NULL before the program has produced one,
 *   after an unwind, and for NULL.
 */
GLTANG_API GLTANG_ValueKind gltang_execution_result_kind(
    const GLTANG_Execution * execution);

/**
 * @brief The result as a boolean.
 *
 * @param execution The execution.
 * @return The value; false unless the result is a boolean that is true.
 */
GLTANG_API bool gltang_execution_result_bool(const GLTANG_Execution * execution);

/**
 * @brief The result as an integer.
 *
 * @param execution The execution.
 * @return The value; 0 unless the result is an integer.
 */
GLTANG_API int64_t gltang_execution_result_integer(
    const GLTANG_Execution * execution);

/**
 * @brief The result as a float.
 *
 * @param execution The execution.
 * @return The value; 0 unless the result is a float.
 */
GLTANG_API double gltang_execution_result_float(
    const GLTANG_Execution * execution);

/**
 * @brief The result's text: a string's bytes (every segment unencoded), or an
 *   error's message.
 *
 * The pointer is valid until the execution next runs. The bytes are
 * NUL-terminated as well as counted.
 *
 * @param execution The execution.
 * @param out_length Receives the byte count; may be NULL.
 * @return The text; NULL when the result is neither a string nor an error.
 */
GLTANG_API const char * gltang_execution_result_text(
    const GLTANG_Execution * execution, size_t * out_length);

/**
 * @brief The result's size: an array's length, a map's entry count, a string's
 *   length in graphemes.
 *
 * @param execution The execution.
 * @return The size; 0 for any other kind.
 */
GLTANG_API size_t gltang_execution_result_size(const GLTANG_Execution * execution);

/**
 * @brief The result as `as string` shows it: `3`, `3.5`, `[1, 2]`, `Error:
 *   Divide by zero`, `[INTEGER TOO LARGE]`.
 *
 * @param execution The execution.
 * @param out_text Receives a NUL-terminated buffer, to release with
 *   ::gltang_buffer_free. Written only on success.
 * @param out_length Receives the byte count; may be NULL.
 * @return ::GLTANG_OK, ::GLTANG_ERR_OOM, or ::GLTANG_ERR_INVALID for NULL.
 */
GLTANG_API GLTANG_Result gltang_execution_result_describe(
    const GLTANG_Execution * execution, char ** out_text, size_t * out_length);

/**
 * @brief Reads the result as an error value, with where it was made.
 *
 * @param execution The execution.
 * @param out_kind Receives the error; may be NULL.
 * @param out_origin Receives where it was created; may be NULL.
 * @return True when the result is an error value; false (and nothing written)
 *   otherwise.
 */
GLTANG_API bool gltang_execution_result_error(const GLTANG_Execution * execution,
    GLTANG_ErrorKind * out_kind, GLTANG_ErrorOrigin * out_origin);

/**
 * @brief One value inside the result, as plain data.
 *
 * The same reading the result accessors give, for an element of an array or a
 * member of a map: a kind, and the member of it that kind has.
 */
typedef struct GLTANG_ResultItem {
  GLTANG_ValueKind kind;  ///< What it is.
  bool boolean;           ///< ::GLTANG_KIND_BOOL.
  int64_t integer;        ///< ::GLTANG_KIND_INTEGER.
  double number;          ///< ::GLTANG_KIND_FLOAT.
  const char * text;      ///< A string's bytes, or an error's message; else NULL.
  size_t length;          ///< The byte count of `text`.
  size_t size;            ///< An array's length, a map's count, a string's graphemes.
  GLTANG_ErrorKind error; ///< ::GLTANG_KIND_ERROR: which one.
} GLTANG_ResultItem;

/**
 * @brief Reads element `index` of an array result.
 *
 * `text` is valid until the execution next runs.
 *
 * @param execution The execution.
 * @param index From zero.
 * @param out_item Receives the element. Written only when this returns true.
 * @return True when the result is an array with that element.
 */
GLTANG_API bool gltang_execution_result_element(const GLTANG_Execution * execution,
    size_t index, GLTANG_ResultItem * out_item);

/**
 * @brief Reads the member of a map result under a key.
 *
 * @param execution The execution.
 * @param key The key, NUL-terminated.
 * @param out_item Receives the member. Written only when this returns true.
 * @return True when the result is a map holding that key.
 */
GLTANG_API bool gltang_execution_result_member(const GLTANG_Execution * execution,
    const char * key, GLTANG_ResultItem * out_item);

/**
 * @brief The raw output: every segment's bytes, unencoded.
 *
 * This is the wrong thing to send to a browser: use
 * ::gltang_execution_output_render for that.
 *
 * @param execution The execution.
 * @param out_length Receives the byte count; may be NULL.
 * @return The bytes, NUL-terminated, owned by the execution and valid until
 *   it next runs; an empty string for NULL.
 */
GLTANG_API const char * gltang_execution_output_raw(
    const GLTANG_Execution * execution, size_t * out_length);

/**
 * @brief The output with every segment encoded per its tag (language
 *   reference, section 8): the thing to send to a browser.
 *
 * Template text outside tags is TRUSTED and passes through unchanged.
 *
 * @param execution The execution.
 * @param out_text Receives a NUL-terminated buffer, to release with
 *   ::gltang_buffer_free. Written only on success.
 * @param out_length Receives the byte count; may be NULL.
 * @return ::GLTANG_OK, ::GLTANG_ERR_OOM, or ::GLTANG_ERR_INVALID for NULL.
 */
GLTANG_API GLTANG_Result gltang_execution_output_render(
    const GLTANG_Execution * execution, char ** out_text, size_t * out_length);

/**
 * @brief Releases a buffer this library returned.
 *
 * @param buffer The buffer, or NULL (ignored).
 */
GLTANG_API void gltang_buffer_free(void * buffer);

/**
 * @brief How many guest frames the unwinder has popped for this execution.
 *
 * Counted by the engine's `unwind` hook, one per frame.
 *
 * @param execution The execution.
 * @return The count; 0 for NULL.
 */
GLTANG_API uint64_t gltang_execution_unwound_frames(
    const GLTANG_Execution * execution);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GLTANG_EXECUTION_H */
