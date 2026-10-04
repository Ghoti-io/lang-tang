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
#include <ghoti.io/runtime-core/b/snapshot.h>

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
 *   running. Called from inside a host callback (a native function or a
 *   factory) it does nothing.
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
 * @brief How an error came to be in the error list.
 */
typedef enum {
  GLTANG_ERROR_HOW_PRINTED = 0,     ///< `print` of an error, which renders as nothing.
  GLTANG_ERROR_HOW_DISCARDED,       ///< An expression statement whose value was an error that nothing holds.
  GLTANG_ERROR_HOW_TEMPLATE_RESULT, ///< The final value of a called template was an error.
  GLTANG_ERROR_HOW_SCOPE_LIMIT,     ///< A template call was stopped by its budget scope.
  GLTANG_ERROR_HOW_CREATED          ///< Logged when created: the host's switch, or the halt option.
} GLTANG_ErrorHow;

/**
 * @brief One entry of the error list.
 *
 * `template_name` is the template the error came from: the main program's name
 * (::gltang_execution_set_name), or the name a template is registered under. The
 * chain of template calls above it is read with ::gltang_execution_error_chain.
 * `file`, `line`, `function` and `offset` are the origin the error carried from
 * where it was created (for a scope limit, where the stopped template was
 * when it was stopped). The strings are owned by the execution and valid until it
 * is destroyed.
 */
typedef struct GLTANG_ErrorEntry {
  GLTANG_ErrorKind kind;        ///< Which error.
  const char * message;         ///< Its text (`Divide by zero`).
  GLTANG_ErrorHow how;          ///< How it got into the list.
  const char * template_name;   ///< The template it came from.
  const char * file;            ///< The origin's file.
  int line;                     ///< The origin's 1-based line; 0 if unknown.
  uint64_t function;            ///< The origin's function index in that program.
  uint64_t offset;              ///< The origin's bytecode offset.
  size_t chain_count;           ///< How many template calls are above it.
} GLTANG_ErrorEntry;

/** @brief One template call above an error: the template, and where in it the call was made. */
typedef struct GLTANG_ErrorLink {
  const char * template_name;   ///< The calling template.
  const char * file;            ///< Its file.
  int line;                     ///< The line of the call.
} GLTANG_ErrorLink;

/**
 * @brief Logs every error value at creation, instead of only the swallowed ones.
 *
 * Errors stay values; the error list (CAP-1) records an error when it is
 * swallowed: printed (and so rendered as nothing), discarded by an expression
 * statement, lost as the final value of a called template, or, for a template
 * call, stopped by its budget scope. With this switch on, every error is entered
 * the moment it is created, as ::GLTANG_ERROR_HOW_CREATED, and the swallow rules
 * add nothing for it. An error is entered at most once.
 *
 * @param execution The execution.
 * @param enabled Whether to log at creation.
 * @return As ::gltang_execution_set_libraries.
 */
GLTANG_API GLTANG_Result gltang_execution_set_log_all_errors(
    GLTANG_Execution * execution, bool enabled);

/**
 * @brief Ends the run at the first error value that is created.
 *
 * The `Limit Exceeded` value of a template call stopped by its scope is not
 * created by an operation: it is entered as ::GLTANG_ERROR_HOW_SCOPE_LIMIT and
 * is subject to neither this option nor ::gltang_execution_set_log_all_errors.
 *
 * The error is entered in the list (as ::GLTANG_ERROR_HOW_CREATED), the run
 * unwinds, and ::grcore_run or ::grcore_resume returns ::GRCORE_ERR_GUEST with
 * the execution ::GLTANG_EXECUTION_UNWOUND. Nothing is printed after the error.
 * It is a request through runtime-core's poll, so a template call's budget scope
 * does not catch it: guest code cannot catch it (AD-5).
 *
 * @param execution The execution.
 * @param enabled Whether to halt.
 * @return As ::gltang_execution_set_libraries; ::GLTANG_ERR_OOM.
 */
GLTANG_API GLTANG_Result gltang_execution_set_halt_on_error(
    GLTANG_Execution * execution, bool enabled);

/**
 * @brief Asks the execution to poll at the start of every statement.
 *
 * Off by default, and then a statement boundary (the `LINE` instruction) does
 * nothing: polls, fuel, pause locations and frame traces are exactly what they
 * are without the instruction. On, each boundary is a poll like a function
 * entry or a loop back-edge, which is what a debugger needs for a breakpoint
 * on a line or a single step. The instruction costs no fuel either way, so the
 * fuel a program is charged does not depend on this option. The engine never
 * asks whether a debugger is attached (AD-2); the host that attaches one turns
 * this on.
 *
 * @param execution The execution.
 * @param enabled Whether statements poll.
 * @return As ::gltang_execution_set_libraries.
 */
GLTANG_API GLTANG_Result gltang_execution_set_statement_polls(
    GLTANG_Execution * execution, bool enabled);

/**
 * @brief The default threshold of ::gltang_execution_set_jit_threshold: the
 *   number of polls a function makes before it tiers up.
 *
 * A compile-time constant; a build that wants another value defines it first.
 */
#ifndef GLTANG_JIT_DEFAULT_THRESHOLD
#define GLTANG_JIT_DEFAULT_THRESHOLD 200u
#endif

/**
 * @brief What the baseline JIT has done for one execution, as plain counters.
 *
 * All zero in a build without the JIT (::gltang_jit_built is false), and for
 * an execution that has not tiered anything up.
 */
typedef struct GLTANG_JitStats {
  uint64_t functions_compiled;  ///< Functions compiled to machine code.
  uint64_t compile_failures;    ///< Compiles that failed (memory, a limit, an unsupported shape); each marks the function never-compile.
  uint64_t functions_discarded; ///< Compiled functions thrown away after eight deoptimizations, never compiled again.
  uint64_t entries;             ///< Entries into compiled code.
  uint64_t returns;             ///< Compiled calls that returned to the interpreter's caller frame.
  uint64_t deopts;              ///< Exits from compiled code to the interpreter at an operation (a failed guard or an operation compiled code leaves to the interpreter).
  uint64_t refused_pauses;      ///< Polls inside compiled code that paused the run.
  uint64_t refused_unwinds;     ///< Polls inside compiled code that unwound the run.
  uint64_t slow_polls;          ///< Polls inside compiled code that took the slow path (a request was pending).
} GLTANG_JitStats;

/**
 * @brief Sets how many polls a function makes before it tiers up (the baseline
 *   JIT, AD-9).
 *
 * Every execution counts the polls each function makes. When a function's
 * count reaches `threshold`, the next poll that continues compiles it, and the
 * interpreter enters the compiled code at the next function entry, or at once
 * if the function was just entered. Compiled code is context-specialised,
 * owned by this execution, and gives the same results, output, errors, fuel
 * and polls as the interpreter (the frame differential and the fuel-parity
 * tests are what say so). The default is ::GLTANG_JIT_DEFAULT_THRESHOLD; 0
 * turns tier-up off for this execution.
 *
 * Stability: like the rest of this header, `free` (tools/check-labels.sh keeps
 * `execution.h` with the engine's other headers). The story that added it asked
 * for `stable`; the gate does not allow that for this file, so a consumer
 * requires the exact version it was built against, as for every function here.
 *
 * @param execution The execution.
 * @param threshold The polls before tier-up, or 0 for never.
 * @return ::GLTANG_OK; ::GLTANG_ERR_INVALID for NULL, an execution that has
 *   started, or a call from inside a host function; ::GLTANG_ERR_OOM when the
 *   execution has no JIT state (its creation could not make it) and making it
 *   now fails; ::GLTANG_ERR_UNSUPPORTED in a build without the JIT (`JIT=no`).
 */
GLTANG_API GLTANG_Result gltang_execution_set_jit_threshold(
    GLTANG_Execution * execution, uint32_t threshold);

/**
 * @brief Reads the execution's JIT counters.
 *
 * @param execution The execution.
 * @param out_stats Receives the counters. Written only on success; all zeros
 *   in a build without the JIT.
 * @return ::GLTANG_OK, or ::GLTANG_ERR_INVALID for a NULL argument.
 */
GLTANG_API GLTANG_Result gltang_execution_jit_stats(
    const GLTANG_Execution * execution, GLTANG_JitStats * out_stats);

/**
 * @brief How many times the engine polled the runtime on behalf of one of its
 *   natives (src/vm/natives.def), by the native's name.
 *
 * A diagnostic for the native budget gate, which uses it to show that the
 * poll of a native is made under that native's name. The names are the ids in
 * natives.def (`STRING_CONCAT`, `ARRAY_GROW`, ...).
 *
 * @param execution The execution.
 * @param native The id.
 * @param out_polls Receives the count. Written only on success.
 * @return ::GLTANG_OK, or ::GLTANG_ERR_INVALID for a NULL argument or a name
 *   that is not an id.
 */
GLTANG_API GLTANG_Result gltang_execution_native_polls(
    const GLTANG_Execution * execution, const char * native, uint64_t * out_polls);

/**
 * @brief Whether this build has the baseline JIT (`JIT=yes`).
 *
 * @return True for a build with it, false for the interpreter-only build.
 */
GLTANG_API bool gltang_jit_built(void);

/**
 * @brief Caps the error list (default 1,024 entries).
 *
 * Entries past the cap are counted in ::gltang_execution_errors_dropped and not
 * stored.
 *
 * @param execution The execution.
 * @param limit The most entries to keep.
 * @return As ::gltang_execution_set_libraries.
 */
GLTANG_API GLTANG_Result gltang_execution_set_error_limit(
    GLTANG_Execution * execution, size_t limit);

/**
 * @brief How many entries the error list holds.
 *
 * @param execution The execution.
 * @return The count; 0 for NULL.
 */
GLTANG_API size_t gltang_execution_error_count(const GLTANG_Execution * execution);

/**
 * @brief Reads an entry of the error list, oldest first.
 *
 * @param execution The execution.
 * @param index From zero.
 * @param out_entry Receives the entry. Written only when this returns true.
 * @return True when there is such an entry.
 */
GLTANG_API bool gltang_execution_error(const GLTANG_Execution * execution,
    size_t index, GLTANG_ErrorEntry * out_entry);

/**
 * @brief How many template calls are above entry `index`.
 *
 * @param execution The execution.
 * @param index The entry.
 * @return The count; 0 for none or out of range.
 */
GLTANG_API size_t gltang_execution_error_chain_count(
    const GLTANG_Execution * execution, size_t index);

/**
 * @brief One template call above an entry, outermost first.
 *
 * @param execution The execution.
 * @param index The entry.
 * @param link From zero.
 * @param out_link Receives the call. Written only when this returns true.
 * @return True when there is such a link.
 */
GLTANG_API bool gltang_execution_error_chain(const GLTANG_Execution * execution,
    size_t index, size_t link, GLTANG_ErrorLink * out_link);

/**
 * @brief How many entries were turned away: by the cap, or because memory ran
 *   short when one was made.
 *
 * @param execution The execution.
 * @return The count; 0 for NULL.
 */
GLTANG_API uint64_t gltang_execution_errors_dropped(
    const GLTANG_Execution * execution);

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
  const char * text;      ///< A string's bytes, an error's message, or a library's name; else NULL.
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


/**
 * @brief A frozen image of an execution that is paused or has not started
 *   (CAP-11): an immutable, reference-counted object that holds no host pointer
 *   and may be restored any number of times, into executions on any thread,
 *   concurrently, and may outlive the execution it came from.
 *
 * It is runtime-core's snapshot (the context's keys write their parts: the
 * guest stack's frames, the heap's objects, and this engine's state), so
 * `grcore_snapshot_retain` and the rest work on it too.
 *
 * *Captured:* the guest stack's frames, the heap's live objects with their
 * stable IDs, the execution's state (`PAUSED` or `NEW`), its roots,
 * temporaries and constants cache, where it paused, the output written so far
 * and the error list (entries, chains and counts), and, by name, every
 * library, native function and template the program holds. The JIT's code and
 * feedback are not captured: the destination's own execution has its own, and a
 * restored run is correct with or without it.
 *
 * *Not captured, supplied again by the host on the destination exactly as for
 * a fresh run:* the group, the options and budgets (fuel used starts at zero in
 * the restored context, and the limits are the destination's), the page
 * provider and allocator, the port and requests, the program (the destination
 * is created for the same program: a different one is refused, by a count of
 * functions and a hash of the content), the libraries
 * (::gltang_execution_set_libraries, before the restore: a library, native
 * function or template the snapshot holds is found again by the library's name
 * and the member's), the seed sequence, the name, the halt and logging
 * switches, the statement polls and the JIT threshold. A generator
 * `random.global` that had been created continues from where it was; one that
 * had not is made from the destination's sequence when first used.
 *
 * *Not done:* a snapshot is an in-memory object. There is no byte format and
 * no file.
 */
typedef GRCORE_Snapshot GLTANG_Snapshot;

/**
 * @brief Takes a snapshot of an execution.
 *
 * Allowed only when the execution is paused, or `NEW` and parked outside `run`,
 * from the context's owning thread, with no host or native frame above `run`, no
 * template call in flight and no open budget scope (AD-20); the heap must hold no
 * C root (`grheap_root_add`), handle, pin or weak cell, and no root source may
 * report a conservative range. A library, native function or template the
 * program holds must be findable again by name: a library with no name, or a
 * name that does not find that library, refuses the snapshot. A template that
 * has already run must be a member of a library the execution can reach.
 * Anything else is ::GLTANG_ERR_INVALID and nothing is changed or left
 * allocated.
 *
 * @param execution The execution.
 * @param out_snapshot Receives the snapshot, with a count of one. Written only
 *   on success.
 * @return ::GLTANG_OK; ::GLTANG_ERR_INVALID as above or for a NULL argument;
 *   ::GLTANG_ERR_LIMIT or ::GLTANG_ERR_OOM when an allocation failed.
 */
GLTANG_API GLTANG_Result gltang_snapshot_take(
    GLTANG_Execution * execution, GLTANG_Snapshot ** out_snapshot);

/**
 * @brief Restores a snapshot into a `NEW` execution.
 *
 * The host creates the destination context, heap and execution as it would for
 * a fresh run (the same program, the same engines registered in the same order, a
 * heap with the same value codec), attaches the libraries, and calls this. If the
 * snapshot was of a paused execution, the destination is then paused at the same
 * place and `grcore_resume` carries on (give it fuel first if the destination's
 * budget is smaller than what the run needs); if it was of a `NEW` one, the
 * destination is `NEW` and `grcore_run` starts it.
 *
 * Instantiating charges the destination's memory budget like any allocation.
 * Any failure, including a mismatch found part way, leaves the destination
 * exactly as it was: a fresh, runnable execution.
 *
 * @param execution The destination, `NEW`.
 * @param snapshot The snapshot.
 * @return ::GLTANG_OK; ::GLTANG_ERR_INVALID for a NULL argument, an execution
 *   that is not `NEW`, a different program, engine table, set of keys or value
 *   codec, a library, native function or template that does not resolve by name,
 *   or a root layout that differs; ::GLTANG_ERR_LIMIT when the destination's
 *   memory or depth budget was too small; ::GLTANG_ERR_OOM.
 */
GLTANG_API GLTANG_Result gltang_snapshot_restore(
    GLTANG_Execution * execution, const GLTANG_Snapshot * snapshot);

/** @brief Adds a reference. Safe from any thread. NULL is a no-op. */
GLTANG_API GLTANG_Snapshot * gltang_snapshot_retain(GLTANG_Snapshot * snapshot);

/** @brief Drops a reference; at zero, frees the snapshot. Safe from any thread. */
GLTANG_API void gltang_snapshot_release(GLTANG_Snapshot * snapshot);

/** @brief The bytes the snapshot holds in all its parts; 0 for NULL. */
GLTANG_API size_t gltang_snapshot_size(const GLTANG_Snapshot * snapshot);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GLTANG_EXECUTION_H */
