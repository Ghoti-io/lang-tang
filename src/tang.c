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
 * The `tang` command: run a template or a script and write what it printed.
 *
 * A host (AD-2): it makes the group, the options, the context and the heap
 * with runtime-core and runtime-heap, compiles the source, and runs it with
 * `grcore_run`. The output is written rendered, every segment encoded per its
 * tag, so `print("<b>" + !"<i>")` writes `<b>&lt;i&gt;`. `--tree` keeps the old
 * behaviour of parsing and dumping the syntax tree.
 *
 * Exit status: 0 the program ran to its end (an empty source is an empty
 * program); 1 the source was refused (a syntax or compile error, printed as
 * `name:line:column: message` on stderr, or a parser limit); 2 a usage error;
 * 3 the source could not be read; 4 out of memory; 5 the run paused (the
 * command has no one to resume it), 6 it was unwound by a limit, 7 the runtime
 * could not be set up, and 8 it was ended by `--halt-on-error` (the run's
 * ERR_GUEST).
 *
 * The host's switches over the error list: `--seed N` is the master seed of the
 * run's random generators, `--log-errors` enters every error when it is created
 * and not only the ones the program swallows, `--halt-on-error` ends the run at
 * the first error, and `--errors` writes the error list to stderr after the
 * run, one `template:file:line: message` an entry, with the chain of template
 * calls above it indented under it.
 *
 * `--dap` makes the command a host of the debugger (story 13): the Debug Adapter
 * Protocol is spoken on stdin and stdout, so the rendered output of the run goes
 * to stderr instead, and the source must come from a file or `--evaluate`. The
 * engine is asked to poll at every statement, a `runtime-debug` debugger is
 * attached, and the host loop is the one of runtime-debug's
 * `examples/dap_session.c`: serve the client until `configurationDone`, run, and
 * at every pause tell the client, serve it, and resume or end the run as it
 * asked. A pause that is not the debugger's (a `--fuel` budget) is shown to the
 * client once, and the next `continue` unwinds the run (exit status 6): the
 * command has no policy for raising a budget. This is one of the two files in
 * the library that may name the debugger (tools/check-edges.sh); the shared
 * object never does.
 */

#include <ghoti.io/lang-tang/macros.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <ghoti.io/cutil/array.h>
#include <ghoti.io/cutil/file.h>
#include <ghoti.io/cutil/memory.h>
#include <ghoti.io/lang-tang/lang-tang.h>
#include <ghoti.io/lang-tang/compile.h>
#include <ghoti.io/lang-tang/execution.h>
#include <ghoti.io/lang-tang/seeds.h>
#include <ghoti.io/runtime-core/runtime-core.h>
#include <ghoti.io/runtime-heap/runtime-heap.h>
#ifdef GLTANG_WITH_DEBUG
#include <ghoti.io/runtime-debug/runtime-debug.h>
#include <signal.h>
#endif
#include <errno.h>
#include <stdlib.h>

#define EXIT_REFUSED 1
#define EXIT_USAGE 2
#define EXIT_READ 3
#define EXIT_MEMORY 4
#define EXIT_PAUSED 5
#define EXIT_UNWOUND 6
#define EXIT_SETUP 7
#define EXIT_GUEST 8

/** ctang's default for the deepest a call may nest (language reference 10.3). */
#define DEFAULT_CALL_DEPTH 512

static void print_help_text(void) {
  printf(
    "Usage: tang [OPTIONS] [FILE]\n"
    "\n"
    "Run a Tang template (or script) and write its output.  With no FILE and no\n"
    "--evaluate, the source is read from stdin.  The output is written rendered:\n"
    "every piece of text is encoded as its tag says.\n"
    "\n"
    "  --evaluate SOURCE, -e SOURCE  Run SOURCE, as a script, instead of a file or stdin\n"
    "  --script, -s                  Run a file or stdin as a script rather than a template\n"
    "  --template, -t                Run the --evaluate source as a template\n"
    "  --tree                        Parse and print the syntax tree; run nothing\n"
    "  --fuel N                      Give the run N units of fuel; if it is not\n"
    "                                finished by then it pauses (exit status 5); a\n"
    "                                limit reached inside one operation (a huge\n"
    "                                repeat or copy) unwinds it instead (exit 6)\n"
    "  --depth N                     Allow calls to nest N deep (default %d)\n"
    "  --seed N                      The master seed of the random generators; two runs\n"
    "                                with one seed draw one sequence (default: entropy)\n"
    "  --log-errors                  Enter every error in the error list when it is\n"
    "                                created, not only the ones the program swallows\n"
    "  --halt-on-error               End the run at the first error (exit status 8)\n"
    "  --errors                      After the run, write the error list to stderr, one\n"
    "                                template:file:line: message per entry, the template\n"
    "                                calls above it indented under it\n"
    "  --dap                         Speak the Debug Adapter Protocol on stdin and stdout\n"
    "                                (breakpoints are set against the file name as given\n"
    "                                here); the run's output goes to stderr. Needs a FILE\n"
    "                                or --evaluate, and not --tree. A --fuel pause is\n"
    "                                shown to the client once; the next continue ends the\n"
    "                                run (exit status 6)\n"
    "  --cleanup, -c                 Accepted for ctang compatibility; this\n"
    "                                command always releases what it allocates\n"
    "  --help, -h                    Display this help message\n"
    "\n"
    "Exit status: 0 ran; 1 refused (name:line:column: message on stderr);\n"
    "2 usage error; 3 the source could not be read; 4 out of memory;\n"
    "5 paused at a poll (reported on stderr); 6 unwound by a limit;\n"
    "7 the runtime could not be set up for a reason other than memory;\n"
    "8 ended by --halt-on-error.\n",
    DEFAULT_CALL_DEPTH);
}


// Reads stdin to its end into a NUL-terminated buffer, returned in *out on
// success. The array owns the length and the growth.
static int read_stdin(char ** out) {
  GCU_Array * input = gcu_array_create(1, 1024, gltang_allocator());
  if (!input) {
    return EXIT_MEMORY;
  }
  char chunk[4096];
  size_t read_count;
  while ((read_count = fread(chunk, 1, sizeof(chunk), stdin)) > 0) {
    if (!gcu_array_append_n(input, chunk, read_count)) {
      gcu_array_destroy(input);
      return EXIT_MEMORY;
    }
  }
  if (ferror(stdin)) {
    gcu_array_destroy(input);
    return EXIT_READ;
  }
  // The terminator is in the buffer but not in the source, which is what lets
  // an empty stdin parse as an empty tree rather than as whatever follows it
  // in memory.
  if (!gcu_array_append(input, "")) {
    gcu_array_destroy(input);
    return EXIT_MEMORY;
  }
  *out = gcu_array_steal(input, NULL);
  gcu_array_destroy(input);
  return 0;
}


static bool parse_count(const char * text, uint64_t * out) {
  if (!text[0]) {
    return false;
  }
  char * end = NULL;
  errno = 0;
  unsigned long long value = strtoull(text, &end, 10);
  if (errno || *end || text[0] == '-') {
    return false;
  }
  *out = (uint64_t)value;
  return true;
}

// Compiles and runs a parsed source, and writes the rendered output. The
// command is a host: it does what any host does (a group, options, a context,
// a heap, an execution, `grcore_run`) and nothing more.
typedef struct Options {
  bool has_fuel;
  uint64_t fuel;
  uint64_t depth;
  bool has_seed;
  uint64_t seed;
  bool log_errors;
  bool halt_on_error;
  bool show_errors;
  bool dap;
} Options;

/** Writes the error list to stderr: `template:file:line: message`, then the chain, indented. */
static void write_errors(const GLTANG_Execution * execution) {
  size_t count = gltang_execution_error_count(execution);
  for (size_t i = 0; i < count; ++i) {
    GLTANG_ErrorEntry entry;
    if (!gltang_execution_error(execution, i, &entry)) {
      continue;
    }
    fprintf(stderr, "%s:%s:%d: %s\n", entry.template_name ? entry.template_name : "?", entry.file ? entry.file : "?", entry.line, entry.message);
    for (size_t k = 0; k < entry.chain_count; ++k) {
      GLTANG_ErrorLink link;
      if (gltang_execution_error_chain(execution, i, k, &link)) {
        fprintf(stderr, "  in %s:%s:%d\n", link.template_name ? link.template_name : "?", link.file ? link.file : "?", link.line);
      }
    }
  }
  uint64_t dropped = gltang_execution_errors_dropped(execution);
  if (dropped) {
    fprintf(stderr, "(%llu more errors not listed)\n", (unsigned long long)dropped);
  }
}


#ifdef GLTANG_WITH_DEBUG
/** The debugger and its session, for `--dap`. All NULL when there is none. */
typedef struct DebugHost {
  GRDBG_Debugger * debugger;
  GRDBG_Transport * transport;
  GRDBG_Dap * dap;
  bool live;      ///< The client is still there to be told things.
  bool detached;  ///< The client said `disconnect` (or went away).
} DebugHost;

static void debug_host_destroy(DebugHost * host) {
  // The session holds the debugger, so it goes before the context does.
  grdbg_dap_destroy(host->dap);
  grdbg_transport_destroy(host->transport);
  memset(host, 0, sizeof(*host));
}

/** Attaches a debugger to the context and opens a DAP session on stdin and stdout. */
static int debug_host_create(DebugHost * host, GRCORE_Context * context, const char * name) {
  GRDBG_Result result = grdbg_debugger_attach(context, NULL, &host->debugger);
  if (result == GRDBG_OK) {
    result = grdbg_transport_create_fd(0, 1, NULL, &host->transport);
  }
  if (result == GRDBG_OK) {
    result = grdbg_dap_create(host->debugger, host->transport, NULL, &host->dap);
  }
  if (result != GRDBG_OK) {
    fprintf(stderr, "%s: the debugger could not be set up: %s\n", name, grdbg_result_string(result));
    debug_host_destroy(host);
    return result == GRDBG_ERR_OOM ? EXIT_MEMORY : EXIT_SETUP;
  }
  host->live = true;
  return 0;
}

/**
 * One `grdbg_dap_serve`. A session that failed has disarmed the debugger and
 * is over: the run goes on free of it, and this says so on stderr.
 */
static GRDBG_ServeResult debug_host_serve(DebugHost * host, const char * name) {
  GRDBG_ServeResult served = GRDBG_SERVE_DETACH;
  if (!host->live) {
    return GRDBG_SERVE_DETACH;
  }
  GRDBG_Result result = grdbg_dap_serve(host->dap, &served);
  if (result != GRDBG_OK) {
    fprintf(stderr, "%s: the debug session ended: %s\n", name, grdbg_result_string(result));
    grdbg_debugger_disarm(host->debugger);
    host->live = false;
    host->detached = true;
    return GRDBG_SERVE_DETACH;
  }
  if (served == GRDBG_SERVE_DETACH) {
    host->detached = true;
  }
  return served;
}

/** True if the pause has a cause the debugger did not vote for (a budget). */
static bool pause_is_a_budget(const GRCORE_Context * context) {
  const GRCORE_Key * ours = grdbg_debugger_key();
  for (size_t k = 0; k < grcore_context_pause_key_count(context); ++k) {
    if (grcore_context_pause_key(context, k) != ours) {
      return true;
    }
  }
  return false;
}

/**
 * The host loop of runtime-debug's examples/dap_session.c: serve until
 * `configurationDone`, run, and at every pause notify, serve and resume (or end
 * the run, as the client asked).
 */
static GRCORE_Result run_debugged(DebugHost * host, GRCORE_Context * context, GLTANG_Execution * execution,
    const char * name, GRCORE_Outcome * outcome) {
  bool terminate = debug_host_serve(host, name) == GRDBG_SERVE_TERMINATE;
  if (terminate) {
    grcore_context_terminate(context);
  }
  GRCORE_Result ran = grcore_run(context, gltang_execution_entry, execution, outcome);
  while (ran == GRCORE_OK && *outcome == GRCORE_OUTCOME_PAUSED) {
    // A budget's pause is shown once; whatever the client answers, the run is
    // unwound after it, because nothing here raises a budget.
    bool unwind = pause_is_a_budget(context);
    if (host->live && grdbg_dap_notify_stopped(host->dap) != GRDBG_OK) {
      grdbg_debugger_disarm(host->debugger);
      host->live = false;
      host->detached = true;
    }
    if (debug_host_serve(host, name) == GRDBG_SERVE_TERMINATE) {
      unwind = true;
    }
    if (unwind) {
      grcore_context_terminate(context);
    }
    ran = grcore_resume(context, outcome);
  }
  return ran;
}

/** Reports the end of the run to the client and answers its `disconnect`. */
static void debug_host_finish(DebugHost * host, const char * name, int status) {
  if (!host->live || host->detached) {
    return;
  }
  if (grdbg_dap_notify_finished(host->dap, status) != GRDBG_OK) {
    host->live = false;
    return;
  }
  (void)debug_host_serve(host, name);
}
#endif

static int run_tree(const GLTANG_Tree * tree, const char * name, const Options * options) {
  GLTANG_ParseError error = {0, 0, {0}};
  GLTANG_Program * program = NULL;
  GLTANG_Result compiled = gltang_compile(tree, name, &error, &program);
  if (compiled == GLTANG_ERR_FORMAT) {
    fprintf(stderr, "%s:%d:%d: %s\n", name, error.line, error.column, error.message);
    return EXIT_REFUSED;
  }
  if (compiled != GLTANG_OK) {
    fprintf(stderr, "%s: %s\n", name, gltang_result_string(compiled));
    return compiled == GLTANG_ERR_OOM ? EXIT_MEMORY : EXIT_REFUSED;
  }

  int status = EXIT_SETUP;
  const char * setup_step = "the runtime could not be set up";
  GRCORE_Group * group = NULL;
  GRCORE_Options * core_options = NULL;
  GRCORE_Context * context = NULL;
  GRHEAP_Options * heap_options = NULL;
  GRHEAP_Heap * heap = NULL;
  GLTANG_Execution * execution = NULL;
  GLTANG_SeedSequence * seeds = NULL;
#ifdef GLTANG_WITH_DEBUG
  DebugHost debug;
  memset(&debug, 0, sizeof(debug));
#endif
  GRCORE_Result step = grcore_group_create(NULL, NULL, &group);
  if (step == GRCORE_OK) {
    step = grcore_options_create(NULL, &core_options);
  }
  GRHEAP_Result heap_step = GRHEAP_OK;
  if (step == GRCORE_OK) {
    heap_step = grheap_options_create(NULL, &heap_options);
  }
  if (step == GRCORE_OK && heap_step == GRHEAP_OK) {
    if (options->has_fuel) {
      step = grcore_options_set_fuel(core_options, options->fuel);
    }
    // ctang's depth counts the calls, and the program's own frame is one more.
    if (step == GRCORE_OK) {
      step = grcore_options_set_guest_depth(core_options, options->depth == UINT64_MAX ? options->depth : options->depth + 1u);
    }
    if (step == GRCORE_OK && gltang_heap_options_configure(heap_options) != GLTANG_OK) {
      step = GRCORE_ERR_INTERNAL;
    }
  }
  if (step == GRCORE_OK && heap_step == GRHEAP_OK) {
    step = grcore_context_create(group, core_options, &context);
    if (step == GRCORE_OK) {
      heap_step = grheap_heap_create(context, heap_options, &heap);
    }
  }
  GLTANG_Result created = GLTANG_OK;
  if (step == GRCORE_OK && heap_step == GRHEAP_OK) {
    created = gltang_execution_create(context, program, &execution);
  }
  if (step != GRCORE_OK || heap_step != GRHEAP_OK || created != GLTANG_OK) {
    if (step == GRCORE_ERR_OOM || heap_step == GRHEAP_ERR_OOM || created == GLTANG_ERR_OOM) {
      status = EXIT_MEMORY;
    }
    else {
      fprintf(stderr, "%s: %s: %s\n", name, setup_step,
          step != GRCORE_OK ? grcore_result_string(step)
          : heap_step != GRHEAP_OK ? grheap_result_string(heap_step)
          : gltang_result_string(created));
    }
    goto done;
  }

  // The host's switches: the main program's name in the error list, the seed
  // sequence, and the two error switches. All are set before the run starts.
  GLTANG_Result configured = gltang_execution_set_name(execution, "main");
  if (configured == GLTANG_OK && options->has_seed) {
    configured = gltang_seeds_create(options->seed, &seeds);
    if (configured == GLTANG_OK) {
      configured = gltang_execution_set_seeds(execution, seeds);
    }
  }
  if (configured == GLTANG_OK) {
    configured = gltang_execution_set_log_all_errors(execution, options->log_errors);
  }
  if (configured == GLTANG_OK) {
    configured = gltang_execution_set_halt_on_error(execution, options->halt_on_error);
  }
  if (configured == GLTANG_OK && options->dap) {
    // A line breakpoint and a step can only stop where the engine polls.
    configured = gltang_execution_set_statement_polls(execution, true);
  }
  if (configured != GLTANG_OK) {
    status = configured == GLTANG_ERR_OOM ? EXIT_MEMORY : EXIT_SETUP;
    if (status == EXIT_SETUP) {
      fprintf(stderr, "%s: %s: %s\n", name, setup_step, gltang_result_string(configured));
    }
    goto done;
  }

  // The DAP stream owns stdout under --dap, so the run's output goes to stderr.
  FILE * out = options->dap ? stderr : stdout;
  GRCORE_Outcome outcome = GRCORE_OUTCOME_FINISHED;
  GRCORE_Result ran;
#ifdef GLTANG_WITH_DEBUG
  if (options->dap) {
    signal(SIGPIPE, SIG_IGN);
    int attached = debug_host_create(&debug, context, name);
    if (attached) {
      status = attached;
      goto done;
    }
    ran = run_debugged(&debug, context, execution, name, &outcome);
  }
  else {
    ran = grcore_run(context, gltang_execution_entry, execution, &outcome);
  }
#else
  ran = grcore_run(context, gltang_execution_entry, execution, &outcome);
#endif
  char * text = NULL;
  size_t length = 0;
  if (gltang_execution_output_render(execution, &text, &length) == GLTANG_OK) {
    fwrite(text, 1, length, out);
    gltang_buffer_free(text);
  }
  else {
    // The output could not be built, so what the program printed is lost:
    // that is the same failure as running out of memory, and not a success.
    fflush(out);
    status = EXIT_MEMORY;
    goto done;
  }
  fflush(out);
  if (ran == GRCORE_OK && outcome == GRCORE_OUTCOME_FINISHED) {
    status = 0;
  }
  else if (ran == GRCORE_OK) {
    GRCORE_Location where = grcore_context_pause_location(context);
    fprintf(stderr, "%s:%d: paused", where.file ? where.file : name, where.line);
    for (size_t k = 0; k < grcore_context_pause_key_count(context); ++k) {
      const GRCORE_Key * key = grcore_context_pause_key(context, k);
      fprintf(stderr, "%s%s", k ? ", " : " on ", key && key->name ? key->name : "?");
    }
    fputc('\n', stderr);
    status = EXIT_PAUSED;
  }
  else if (ran == GRCORE_ERR_GUEST) {
    fprintf(stderr, "%s: the run was ended by an error\n", name);
    status = EXIT_GUEST;
  }
  else {
    fprintf(stderr, "%s: the run was stopped: %s\n", name, grcore_result_string(ran));
    status = EXIT_UNWOUND;
  }
  if (options->show_errors) {
    write_errors(execution);
  }
#ifdef GLTANG_WITH_DEBUG
  if (options->dap) {
    debug_host_finish(&debug, name, status);
  }
#endif

done:
  if (status == EXIT_MEMORY) {
    fprintf(stderr, "%s: out of memory\n", name);
  }
#ifdef GLTANG_WITH_DEBUG
  debug_host_destroy(&debug);
#endif
  if (context) {
    grcore_context_destroy(context);
  }
  grheap_options_destroy(heap_options);
  grcore_options_destroy(core_options);
  gltang_seeds_destroy(seeds);
  if (group) {
    grcore_group_destroy(group);
  }
  gltang_program_release(program);
  return status;
}


int main(int argc, const char * argv[]) {
  const char * file_name = NULL;
  const char * eval = NULL;
  bool is_script = false;
  bool is_template = false;
  bool dump_tree = false;
  Options options;
  memset(&options, 0, sizeof(options));
  options.depth = DEFAULT_CALL_DEPTH;

  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--evaluate") || !strcmp(argv[i], "-e")) {
      if (i + 1 >= argc) {
        fprintf(stderr, "tang: %s needs an argument\n", argv[i]);
        return EXIT_USAGE;
      }
      eval = argv[++i];
    }
    else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
      print_help_text();
      return 0;
    }
    else if (!strcmp(argv[i], "--script") || !strcmp(argv[i], "-s")) {
      is_script = true;
    }
    else if (!strcmp(argv[i], "--template") || !strcmp(argv[i], "-t")) {
      is_template = true;
    }
    else if (!strcmp(argv[i], "--cleanup") || !strcmp(argv[i], "-c")) {
      // Accepted and ignored: see print_help_text().
    }
    else if (!strcmp(argv[i], "--tree")) {
      dump_tree = true;
    }
    else if (!strcmp(argv[i], "--fuel") || !strcmp(argv[i], "--depth") || !strcmp(argv[i], "--seed")) {
      bool is_fuel = !strcmp(argv[i], "--fuel");
      bool is_seed = !strcmp(argv[i], "--seed");
      uint64_t * target = is_fuel ? &options.fuel : (is_seed ? &options.seed : &options.depth);
      if (i + 1 >= argc || !parse_count(argv[i + 1], target)) {
        fprintf(stderr, "tang: %s needs a number\n", argv[i]);
        return EXIT_USAGE;
      }
      options.has_fuel = options.has_fuel || is_fuel;
      options.has_seed = options.has_seed || is_seed;
      ++i;
    }
    else if (!strcmp(argv[i], "--halt-on-error")) {
      options.halt_on_error = true;
    }
    else if (!strcmp(argv[i], "--log-errors")) {
      options.log_errors = true;
    }
    else if (!strcmp(argv[i], "--errors")) {
      options.show_errors = true;
    }
    else if (!strcmp(argv[i], "--dap")) {
      options.dap = true;
    }
    else if (argv[i][0] == '-' && argv[i][1] != '\0') {
      fprintf(stderr, "tang: unknown option %s\n", argv[i]);
      return EXIT_USAGE;
    }
    else if (!file_name) {
      file_name = argv[i];
    }
    else {
      fprintf(stderr, "tang: too many arguments\n");
      return EXIT_USAGE;
    }
  }
  if (eval && file_name) {
    fprintf(stderr, "tang: give either a file name or --evaluate, not both\n");
    return EXIT_USAGE;
  }
  if (is_script && is_template) {
    fprintf(stderr, "tang: give either --script or --template, not both\n");
    return EXIT_USAGE;
  }
  if (options.dap) {
#ifndef GLTANG_WITH_DEBUG
    fprintf(stderr, "tang: --dap is not available: this tang was built without runtime-debug\n");
    return EXIT_USAGE;
#endif
    if (dump_tree) {
      fprintf(stderr, "tang: --dap and --tree cannot be combined: --tree runs nothing to debug\n");
      return EXIT_USAGE;
    }
    if (!eval && !file_name) {
      fprintf(stderr, "tang: --dap needs a FILE or --evaluate: standard input carries the debug session\n");
      return EXIT_USAGE;
    }
  }
  // The source given with --evaluate is code (`tang -e 'print(1+2);'` prints
  // 3); a file or stdin is a template unless it is told otherwise.
  if (eval && !is_template) {
    is_script = true;
  }

  char * buffer = NULL;
  const char * name = "<stdin>";
  const char * source = eval;
  if (eval) {
    name = "<evaluate>";
  }
  else if (file_name) {
    // cutil reads the whole file, in chunks rather than by seeking to the end
    // first, so this works on a pipe as well as on an ordinary file, and puts
    // a NUL one past the end.
    void * contents = NULL;
    size_t length = 0;
    GCU_File_Result result = gcu_file_read(file_name, GCU_FILE_UNLIMITED, gltang_allocator(), &contents, &length);
    if (result != GCU_FILE_OK) {
      fprintf(stderr, "tang: failed to read the file %s: %s\n", file_name, gcu_file_result_string(result));
      return EXIT_READ;
    }
    buffer = contents;
    source = buffer;
    name = file_name;
    // The parser takes a NUL-terminated string, so a NUL inside the file would
    // end the source early and the rest would be dropped without a word.
    if (strlen(buffer) != length) {
      fprintf(stderr, "tang: %s contains a NUL byte\n", file_name);
      gcu_free(buffer);
      return EXIT_READ;
    }
  }
  else {
    int failure = read_stdin(&buffer);
    if (failure) {
      fprintf(stderr, failure == EXIT_READ ? "tang: failed to read from stdin\n" : "tang: out of memory\n");
      return failure;
    }
    source = buffer;
  }

  GLTANG_Tree * tree = NULL;
  GLTANG_ParseError error = {0, 0, {0}};
  GLTANG_Result result = gltang_parse(source, is_script ? GLTANG_PARSE_SCRIPT : GLTANG_PARSE_TEMPLATE, &error, &tree);
  int status = 0;
  if (result == GLTANG_OK) {
    if (dump_tree) {
      gltang_tree_print(tree);
    }
    else {
      status = run_tree(tree, name, &options);
    }
    gltang_tree_destroy(tree);
  }
  else if (result == GLTANG_ERR_FORMAT) {
    fprintf(stderr, "%s:%d:%d: %s\n", name, error.line, error.column, error.message);
    status = EXIT_REFUSED;
  }
  else {
    fprintf(stderr, "%s: %s\n", name, gltang_result_string(result));
    status = (result == GLTANG_ERR_OOM) ? EXIT_MEMORY : EXIT_REFUSED;
  }
  if (buffer) {
    gcu_free(buffer);
  }
  return status;
}
