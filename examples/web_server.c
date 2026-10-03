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
 * A web server that serves templates, one context per request, and can be
 * debugged: the milestone's success signal in one program (story 13).
 *
 * `GET /<name>` runs `<templates-dir>/<name>.tang` in a context of its own, made
 * for that request from one shared group and destroyed after it, under a fuel
 * budget. A runaway template does not take the server with it: the budget pauses
 * it, the host raises the budget once and resumes, and a second pause is final,
 * so the response is `503` naming the template's file and the line it was on
 * (`paused at examples/web/runaway.tang:3`), and the next request is served as
 * if nothing had happened. `/slow` shows the other outcome: it needs the raise,
 * and finishes.
 *
 * `GET /<name>?debug=1` makes the host accept one connection on a second
 * loopback socket (`--debug-port`) and run that request's context under a
 * debugger: statement polls on, a runtime-debug debugger attached, a Debug
 * Adapter Protocol session on the connection, and the host loop of
 * runtime-debug's `examples/dap_session.c`. A client sets a breakpoint in a
 * template, reads the variables, steps in, over and out, and continues, and the
 * response carries the body an undebugged request would have had.
 *
 * Statuses: 200 finished (with `X-Template-Errors`, the length of the error
 * list); 404 no such template; 400 not a request this server reads; 500 the
 * template was refused, with the error as the body (`file:line:col: message`);
 * 503 the run was stopped (`paused at <file>:<line>`, `unwound by a limit`, or
 * `terminated by the debugger`); 504 `no debugger connected` in time.
 *
 * This is a demonstration, not a policy. It is a plain accept loop on one
 * thread, binds `127.0.0.1` only, speaks HTTP/1.1 with `Connection: close`, reads
 * no request bodies, and has no TLS. The libraries never listen or accept
 * (AD-2): this host does, and the debug socket it opens is a way to show a
 * session, not a design for debugging a server over a network, which this
 * milestone does not offer.
 *
 * `--self-test` (what `make examples` runs) starts both listeners on ephemeral
 * ports, plays a client against them from other threads, checks every
 * behaviour above, and exits 0 only if all hold. Without it the server runs
 * until SIGINT or SIGTERM.
 *
 *     web_server [--port N] [--debug-port N] [--templates DIR] [--fuel N]
 *                [--raise N] [--debug-wait-ms N] [--self-test]
 */

#define _POSIX_C_SOURCE 200809L

#include <ghoti.io/lang-tang/compile.h>
#include <ghoti.io/lang-tang/execution.h>
#include <ghoti.io/lang-tang/lang-tang.h>
#include <ghoti.io/lang-tang/library.h>
#include <ghoti.io/lang-tang/program.h>
#include <ghoti.io/runtime-core/runtime-core.h>
#include <ghoti.io/runtime-debug/runtime-debug.h>
#include <ghoti.io/runtime-heap/runtime-heap.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* ---- limits and defaults ------------------------------------------------ */

#define REQUEST_LINE_MAX 4096u
#define HEADERS_MAX 16384u
#define NAME_MAX_LENGTH 64u
#define TEMPLATE_MAX_BYTES (1024u * 1024u)
#define READ_DEADLINE_MS 5000
#define DEFAULT_FUEL 200000u
#define DEFAULT_DEBUG_WAIT_MS 10000
#define NESTED_SCOPE_FUEL 20000u

/** The templates the page calls by name, registered with budgets of their own. */
static const char * const NESTED[] = {"sidebar", "layout"};

/* Checked in every build: an example that asserts nothing under NDEBUG is not
 * an example of anything. */
#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "web_server: check failed at line %d: %s\n", __LINE__, \
          #condition);                                                         \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

static atomic_int g_stop;

static void on_signal(int signal_number) {
  (void)signal_number;
  atomic_store(&g_stop, 1);
}

/* ---- a growing string --------------------------------------------------- */

typedef struct {
  char * data;
  size_t length;
  size_t capacity;
} Buf;

static bool buf_append(Buf * buf, const char * bytes, size_t n) {
  if (buf->length + n + 1 > buf->capacity) {
    size_t capacity = (buf->length + n + 1) * 2;
    char * grown = realloc(buf->data, capacity);
    if (!grown) {
      return false;
    }
    buf->data = grown;
    buf->capacity = capacity;
  }
  memcpy(buf->data + buf->length, bytes, n);
  buf->length += n;
  buf->data[buf->length] = '\0';
  return true;
}

static bool buf_printf(Buf * buf, const char * format, ...) __attribute__((format(printf, 2, 3)));
static bool buf_printf(Buf * buf, const char * format, ...) {
  va_list args;
  va_start(args, format);
  va_list again;
  va_copy(again, args);
  int n = vsnprintf(NULL, 0, format, args);
  va_end(args);
  bool ok = false;
  if (n >= 0) {
    char * text = malloc((size_t)n + 1);
    if (text) {
      vsnprintf(text, (size_t)n + 1, format, again);
      ok = buf_append(buf, text, (size_t)n);
      free(text);
    }
  }
  va_end(again);
  return ok;
}

/* ---- configuration and the server --------------------------------------- */

typedef struct {
  const char * templates;
  int port;
  int debug_port;
  bool has_debug;
  uint64_t fuel;
  uint64_t raise;
  bool has_raise;
  int debug_wait_ms;
  bool self_test;
} Config;

typedef struct {
  Config config;
  int listener;
  int debug_listener;
  int port;        /* what the listeners are bound to, after an ephemeral bind */
  int debug_port;
  GRCORE_Group * group;
  GLTANG_Library * library;
  long contexts; /* contexts made: one for each request that ran a template */
} Server;

/* ---- responses ---------------------------------------------------------- */

typedef struct {
  int status;
  Buf body;
  long errors;      /* >= 0: X-Template-Errors */
  long context_id;  /* > 0: X-Context-Id */
  bool raised;      /* X-Fuel-Raised: 1 */
} Response;

static const char * reason_phrase(int status) {
  switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return "Unknown";
  }
}

static void response_init(Response * r) {
  memset(r, 0, sizeof(*r));
  r->errors = -1;
}

static void response_say(Response * r, int status, const char * format, ...) __attribute__((format(printf, 3, 4)));
static void response_say(Response * r, int status, const char * format, ...) {
  r->status = status;
  r->body.length = 0;
  va_list args;
  va_start(args, format);
  va_list again;
  va_copy(again, args);
  int n = vsnprintf(NULL, 0, format, args);
  va_end(args);
  if (n >= 0) {
    char * text = malloc((size_t)n + 1);
    if (text) {
      vsnprintf(text, (size_t)n + 1, format, again);
      buf_append(&r->body, text, (size_t)n);
      free(text);
    }
  }
  va_end(again);
}

static bool send_all(int fd, const char * bytes, size_t n) {
  while (n > 0) {
    ssize_t sent = send(fd, bytes, n, MSG_NOSIGNAL);
    if (sent < 0 && errno == EINTR) {
      continue;
    }
    if (sent <= 0) {
      return false;
    }
    bytes += sent;
    n -= (size_t)sent;
  }
  return true;
}

static void write_response(int fd, const Response * r) {
  Buf head = {NULL, 0, 0};
  buf_printf(&head, "HTTP/1.1 %d %s\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %zu\r\nConnection: close\r\n",
      r->status, reason_phrase(r->status), r->body.length);
  if (r->errors >= 0) {
    buf_printf(&head, "X-Template-Errors: %ld\r\n", r->errors);
  }
  if (r->context_id > 0) {
    buf_printf(&head, "X-Context-Id: %ld\r\n", r->context_id);
  }
  if (r->raised) {
    buf_append(&head, "X-Fuel-Raised: 1\r\n", 18);
  }
  buf_append(&head, "\r\n", 2);
  if (head.data) {
    send_all(fd, head.data, head.length);
  }
  if (r->body.length > 0) {
    send_all(fd, r->body.data, r->body.length);
  }
  free(head.data);
}

/* ---- reading a request -------------------------------------------------- */

typedef struct {
  char name[NAME_MAX_LENGTH + 1];
  bool name_valid; /* [a-z0-9_-]+ and short; anything else is a 404 */
  bool debug;      /* the query has debug=1 */
} Request;

static long now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (long)t.tv_sec * 1000 + (long)(t.tv_nsec / 1000000);
}

/** Waits for `fd` to be readable for up to `timeout_ms`. */
static bool wait_readable(int fd, int timeout_ms) {
  struct pollfd p = {fd, POLLIN, 0};
  for (;;) {
    int ready = poll(&p, 1, timeout_ms);
    if (ready < 0 && errno == EINTR) {
      continue;
    }
    return ready > 0;
  }
}

static bool valid_name(const char * name, size_t length) {
  if (length == 0 || length > NAME_MAX_LENGTH) {
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    char c = name[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
      return false;
    }
  }
  return true;
}

/**
 * Reads the request line and the headers (no body) with a bound on the size of
 * each and on the time: line 4 KiB, headers 16 KiB. Returns 0 and fills the
 * request, or the status to answer with.
 */
static int read_request(int fd, Request * request) {
  static char buffer[REQUEST_LINE_MAX + HEADERS_MAX + 8];
  size_t have = 0;
  long deadline = now_ms() + READ_DEADLINE_MS;
  char * end = NULL;
  for (;;) {
    buffer[have] = '\0';
    end = strstr(buffer, "\r\n\r\n");
    if (end) {
      break;
    }
    char * line_end = strstr(buffer, "\r\n");
    if (!line_end && have > REQUEST_LINE_MAX) {
      return 400; /* a request line that does not end within its cap */
    }
    if (have >= sizeof(buffer) - 1) {
      return 400; /* headers over their cap */
    }
    long left = deadline - now_ms();
    if (left <= 0 || !wait_readable(fd, (int)left)) {
      return 400;
    }
    ssize_t n = recv(fd, buffer + have, sizeof(buffer) - 1 - have, 0);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      return 400; /* the client went away mid-request */
    }
    have += (size_t)n;
  }
  char * line_end = strstr(buffer, "\r\n");
  size_t line_length = (size_t)(line_end - buffer);
  if (line_length > REQUEST_LINE_MAX || (size_t)(end - line_end) > HEADERS_MAX) {
    return 400;
  }
  /* "GET <target> HTTP/1.x" */
  *line_end = '\0';
  char * method_end = strchr(buffer, ' ');
  if (!method_end || (size_t)(method_end - buffer) != 3 || strncmp(buffer, "GET", 3) != 0) {
    return 400;
  }
  char * target = method_end + 1;
  char * target_end = strchr(target, ' ');
  if (!target_end || target_end == target || strncmp(target_end + 1, "HTTP/1.", 7) != 0
      || (target_end[8] != '0' && target_end[8] != '1') || target_end[9] != '\0') {
    return 400;
  }
  *target_end = '\0';
  if (target[0] != '/') {
    return 400;
  }
  /* Each header is "name: value": anything else is not a request. */
  *line_end = '\r';
  for (char * line = line_end + 2; line < end + 2;) {
    char * next = strstr(line, "\r\n");
    if (!next) {
      break;
    }
    if (next == line) {
      break;
    }
    char * colon = memchr(line, ':', (size_t)(next - line));
    if (!colon || colon == line) {
      return 400;
    }
    line = next + 2;
  }
  memset(request, 0, sizeof(*request));
  char * query = strchr(target, '?');
  size_t path_length = query ? (size_t)(query - target) : strlen(target);
  const char * name = target + 1;
  size_t name_length = path_length - 1;
  request->name_valid = valid_name(name, name_length);
  if (request->name_valid) {
    memcpy(request->name, name, name_length);
    request->name[name_length] = '\0';
  }
  if (query) {
    for (char * param = query + 1; param;) {
      char * amp = strchr(param, '&');
      size_t length = amp ? (size_t)(amp - param) : strlen(param);
      if (length == 7 && strncmp(param, "debug=1", 7) == 0) {
        request->debug = true;
      }
      param = amp ? amp + 1 : NULL;
    }
  }
  return 0;
}

/* ---- templates ---------------------------------------------------------- */

typedef enum { LOAD_OK, LOAD_MISSING, LOAD_REFUSED, LOAD_FAILED } Load;

/**
 * Reads, parses and compiles a template. The compile `file` is `path`, exactly,
 * so a pause or a breakpoint names the file the host read it from.
 */
static Load load_program(const char * path, GLTANG_Program ** out, Buf * message) {
  FILE * file = fopen(path, "rb");
  if (!file) {
    return errno == ENOENT || errno == ENOTDIR ? LOAD_MISSING : LOAD_FAILED;
  }
  char * source = malloc(TEMPLATE_MAX_BYTES + 1);
  if (!source) {
    fclose(file);
    return LOAD_FAILED;
  }
  size_t length = fread(source, 1, TEMPLATE_MAX_BYTES + 1, file);
  bool failed = ferror(file) != 0;
  fclose(file);
  if (failed || length > TEMPLATE_MAX_BYTES) {
    free(source);
    buf_printf(message, "%s: unreadable or over %u bytes", path, TEMPLATE_MAX_BYTES);
    return LOAD_FAILED;
  }
  source[length] = '\0';
  if (strlen(source) != length) {
    free(source);
    buf_printf(message, "%s: contains a NUL byte", path);
    return LOAD_REFUSED;
  }
  GLTANG_Tree * tree = NULL;
  GLTANG_ParseError error = {0, 0, {0}};
  GLTANG_Result result = gltang_parse(source, GLTANG_PARSE_TEMPLATE, &error, &tree);
  free(source);
  if (result == GLTANG_ERR_FORMAT) {
    buf_printf(message, "%s:%d:%d: %s", path, error.line, error.column, error.message);
    return LOAD_REFUSED;
  }
  if (result != GLTANG_OK) {
    buf_printf(message, "%s: %s", path, gltang_result_string(result));
    return LOAD_FAILED;
  }
  GLTANG_Program * program = NULL;
  result = gltang_compile(tree, path, &error, &program);
  gltang_tree_destroy(tree);
  if (result == GLTANG_ERR_FORMAT) {
    buf_printf(message, "%s:%d:%d: %s", path, error.line, error.column, error.message);
    return LOAD_REFUSED;
  }
  if (result != GLTANG_OK) {
    buf_printf(message, "%s: %s", path, gltang_result_string(result));
    return LOAD_FAILED;
  }
  *out = program;
  return LOAD_OK;
}

static void template_path(const Server * server, const char * name, char * out, size_t size) {
  snprintf(out, size, "%s/%s.tang", server->config.templates, name);
}

/** The templates `use` binds for every request, each with a scope budget of its own. */
static bool build_library(Server * server) {
  if (gltang_library_create(NULL, &server->library) != GLTANG_OK) {
    return false;
  }
  for (size_t i = 0; i < sizeof(NESTED) / sizeof(NESTED[0]); ++i) {
    char path[1024];
    template_path(server, NESTED[i], path, sizeof(path));
    GLTANG_Program * program = NULL;
    Buf message = {NULL, 0, 0};
    Load loaded = load_program(path, &program, &message);
    if (loaded != LOAD_OK) {
      if (loaded == LOAD_MISSING) {
        fprintf(stderr, "web_server: no template %s (is --templates right?)\n", path);
      }
      else {
        fprintf(stderr, "web_server: %s\n", message.data ? message.data : "a nested template could not be loaded");
      }
      free(message.data);
      return false;
    }
    GLTANG_Result added = gltang_library_add_template(server->library, NESTED[i], program, NESTED_SCOPE_FUEL, GLTANG_SCOPE_EMPTY);
    gltang_program_release(program); /* the library retains it */
    if (added != GLTANG_OK) {
      return false;
    }
  }
  return true;
}

/* ---- the debugger for one request --------------------------------------- */

typedef struct {
  GRDBG_Debugger * debugger;
  GRDBG_Transport * transport;
  GRDBG_Dap * dap;
  int fd;
  bool live;      /* the client is still there to be told things */
  bool detached;  /* the client said disconnect, or went away */
} Debug;

static void debug_destroy(Debug * debug) {
  /* The session holds the debugger: it goes before the context does. */
  grdbg_dap_destroy(debug->dap);
  grdbg_transport_destroy(debug->transport);
  if (debug->fd >= 0) {
    close(debug->fd);
  }
  memset(debug, 0, sizeof(*debug));
  debug->fd = -1;
}

static bool debug_create(Debug * debug, GRCORE_Context * context, int fd) {
  debug->fd = fd;
  if (grdbg_debugger_attach(context, NULL, &debug->debugger) != GRDBG_OK
      || grdbg_transport_create_fd(fd, fd, NULL, &debug->transport) != GRDBG_OK
      || grdbg_dap_create(debug->debugger, debug->transport, NULL, &debug->dap) != GRDBG_OK) {
    debug_destroy(debug);
    return false;
  }
  debug->live = true;
  return true;
}

static void debug_lost(Debug * debug) {
  grdbg_debugger_disarm(debug->debugger);
  debug->live = false;
  debug->detached = true;
}

static GRDBG_ServeResult debug_serve(Debug * debug) {
  GRDBG_ServeResult served = GRDBG_SERVE_DETACH;
  if (!debug->live) {
    return GRDBG_SERVE_DETACH;
  }
  GRDBG_Result result = grdbg_dap_serve(debug->dap, &served);
  if (result != GRDBG_OK) {
    debug_lost(debug);
    return GRDBG_SERVE_DETACH;
  }
  if (served == GRDBG_SERVE_DETACH) {
    debug->detached = true;
  }
  return served;
}

static bool pause_is_a_budget(const GRCORE_Context * context) {
  const GRCORE_Key * ours = grdbg_debugger_key();
  for (size_t k = 0; k < grcore_context_pause_key_count(context); ++k) {
    if (grcore_context_pause_key(context, k) != ours) {
      return true;
    }
  }
  return false;
}

/* ---- one request -------------------------------------------------------- */

/**
 * Runs a compiled template in a context made for this request and builds the
 * response. `debug_fd` is a connected debug socket (the request owns it from
 * here) or -1. The host survives every outcome: nothing here exits.
 */
static void run_request(Server * server, const char * path, GLTANG_Program * program, int debug_fd, Response * response) {
  GRCORE_Options * core_options = NULL;
  GRHEAP_Options * heap_options = NULL;
  GRCORE_Context * context = NULL;
  GRHEAP_Heap * heap = NULL;
  GLTANG_Execution * execution = NULL;
  Debug debug;
  memset(&debug, 0, sizeof(debug));
  debug.fd = -1;
  bool debugged = debug_fd >= 0;

  if (grcore_options_create(NULL, &core_options) != GRCORE_OK
      || grheap_options_create(NULL, &heap_options) != GRHEAP_OK
      || grcore_options_set_fuel(core_options, server->config.fuel) != GRCORE_OK
      || gltang_heap_options_configure(heap_options) != GLTANG_OK
      || grcore_context_create(server->group, core_options, &context) != GRCORE_OK
      || grheap_heap_create(context, heap_options, &heap) != GRHEAP_OK
      || gltang_execution_create(context, program, &execution) != GLTANG_OK
      || gltang_execution_set_libraries(execution, server->library) != GLTANG_OK
      || gltang_execution_set_name(execution, "page") != GLTANG_OK
      || (debugged && gltang_execution_set_statement_polls(execution, true) != GLTANG_OK)) {
    response_say(response, 500, "the runtime could not be set up for this request\n");
    if (debug_fd >= 0) {
      close(debug_fd);
    }
    goto done;
  }
  response->context_id = ++server->contexts;

  if (debugged && !debug_create(&debug, context, debug_fd)) {
    response_say(response, 500, "the debugger could not be set up for this request\n");
    goto done;
  }

  /* Serve the client until configurationDone (its breakpoints are set before). */
  bool terminated_by_client = debugged && debug_serve(&debug) == GRDBG_SERVE_TERMINATE;
  if (terminated_by_client) {
    grcore_context_terminate(context);
  }

  GRCORE_Outcome outcome = GRCORE_OUTCOME_FINISHED;
  GRCORE_Result ran = grcore_run(context, gltang_execution_entry, execution, &outcome);
  bool raised = false;
  bool final_pause = false;
  char final_where[1100];
  final_where[0] = '\0';
  while (ran == GRCORE_OK && outcome == GRCORE_OUTCOME_PAUSED) {
    bool budget = pause_is_a_budget(context);
    GRCORE_Location where = grcore_context_pause_location(context);
    if (debug.live && grdbg_dap_notify_stopped(debug.dap) != GRDBG_OK) {
      debug_lost(&debug);
    }
    bool terminate = false;
    if (debugged && debug_serve(&debug) == GRDBG_SERVE_TERMINATE) {
      terminated_by_client = true;
      terminate = true;
    }
    if (!terminate && budget) {
      if (!raised) {
        /* The policy of this host, not the library's: raise the budget once. */
        raised = true;
        grcore_context_set_fuel(context, grcore_context_fuel_used(context) + server->config.raise);
      }
      else {
        /* A second pause is final: say where, and unwind the run. */
        final_pause = true;
        snprintf(final_where, sizeof(final_where), "%s:%d", where.file ? where.file : path, where.line);
        terminate = true;
      }
    }
    if (terminate) {
      grcore_context_terminate(context);
    }
    ran = grcore_resume(context, &outcome);
  }
  response->raised = raised;

  if (terminated_by_client) {
    response_say(response, 503, "terminated by the debugger\n");
  }
  else if (final_pause) {
    response_say(response, 503, "paused at %s\n", final_where);
  }
  else if (ran == GRCORE_OK && outcome == GRCORE_OUTCOME_FINISHED) {
    char * text = NULL;
    size_t length = 0;
    if (gltang_execution_output_render(execution, &text, &length) == GLTANG_OK) {
      response->status = 200;
      response->body.length = 0;
      buf_append(&response->body, text, length);
      response->errors = (long)gltang_execution_error_count(execution);
      gltang_buffer_free(text);
    }
    else {
      response_say(response, 500, "the output could not be built\n");
    }
  }
  else if (ran == GRCORE_ERR_LIMIT) {
    response_say(response, 503, "unwound by a limit\n");
  }
  else {
    response_say(response, 500, "the run failed: %s\n", grcore_result_string(ran));
  }

  /* Tell the client how it ended, and answer its disconnect. */
  if (debug.live && !debug.detached) {
    if (grdbg_dap_notify_finished(debug.dap, response->status == 200 ? 0 : 1) == GRDBG_OK) {
      (void)debug_serve(&debug);
    }
  }

done:
  debug_destroy(&debug);
  if (context) {
    grcore_context_destroy(context);
  }
  grheap_options_destroy(heap_options);
  grcore_options_destroy(core_options);
}

/** Waits (bounded) for a debugger to connect to the debug listener. */
static int accept_debugger(Server * server) {
  if (server->debug_listener < 0) {
    return -1;
  }
  long deadline = now_ms() + server->config.debug_wait_ms;
  for (;;) {
    long left = deadline - now_ms();
    if (left <= 0 || atomic_load(&g_stop)) {
      return -1;
    }
    if (!wait_readable(server->debug_listener, left > 100 ? 100 : (int)left)) {
      continue;
    }
    int fd = accept(server->debug_listener, NULL, NULL);
    if (fd >= 0) {
      return fd;
    }
    if (errno != EINTR && errno != EAGAIN && errno != ECONNABORTED) {
      return -1;
    }
  }
}

static void handle_connection(Server * server, int fd) {
  Response response;
  response_init(&response);
  Request request;
  int bad = read_request(fd, &request);
  if (bad) {
    response_say(&response, bad, "bad request\n");
  }
  else if (!request.name_valid) {
    response_say(&response, 404, "no such template\n");
  }
  else {
    char path[1024];
    template_path(server, request.name, path, sizeof(path));
    GLTANG_Program * program = NULL;
    Buf message = {NULL, 0, 0};
    Load loaded = load_program(path, &program, &message);
    if (loaded == LOAD_MISSING) {
      response_say(&response, 404, "no such template\n");
    }
    else if (loaded == LOAD_REFUSED) {
      response_say(&response, 500, "%s\n", message.data ? message.data : "refused");
    }
    else if (loaded != LOAD_OK) {
      response_say(&response, 500, "%s\n", message.data ? message.data : "the template could not be read");
    }
    else if (request.debug && server->debug_listener < 0) {
      response_say(&response, 400, "debugging is not enabled (start the server with --debug-port)\n");
    }
    else {
      int debug_fd = -1;
      if (request.debug) {
        debug_fd = accept_debugger(server);
      }
      if (request.debug && debug_fd < 0) {
        response_say(&response, 504, "no debugger connected\n");
      }
      else {
        run_request(server, path, program, debug_fd, &response);
      }
    }
    free(message.data);
    gltang_program_release(program);
  }
  printf("web_server: %s -> %d\n", bad ? "(bad request)" : (request.name_valid ? request.name : "(unknown)"), response.status);
  fflush(stdout);
  write_response(fd, &response);
  free(response.body.data);
}

/* ---- the listeners and the loop ----------------------------------------- */

/** Binds a loopback listener; `port` 0 is ephemeral. Returns the fd, and the port in `*bound`. */
static int listen_loopback(int port, int * bound) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* 127.0.0.1 only */
  address.sin_port = htons((uint16_t)port);
  socklen_t length = sizeof(address);
  if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(fd, 16) != 0
      || getsockname(fd, (struct sockaddr *)&address, &length) != 0) {
    close(fd);
    return -1;
  }
  *bound = ntohs(address.sin_port);
  return fd;
}

static void serve_loop(Server * server) {
  while (!atomic_load(&g_stop)) {
    if (!wait_readable(server->listener, 100)) {
      continue;
    }
    int fd = accept(server->listener, NULL, NULL);
    if (fd < 0) {
      continue;
    }
    handle_connection(server, fd);
    close(fd);
  }
}

static bool server_open(Server * server) {
  server->listener = listen_loopback(server->config.port, &server->port);
  server->debug_listener = -1;
  if (server->listener < 0) {
    fprintf(stderr, "web_server: cannot listen on 127.0.0.1:%d: %s\n", server->config.port, strerror(errno));
    return false;
  }
  if (server->config.has_debug) {
    server->debug_listener = listen_loopback(server->config.debug_port, &server->debug_port);
    if (server->debug_listener < 0) {
      fprintf(stderr, "web_server: cannot listen on 127.0.0.1:%d: %s\n", server->config.debug_port, strerror(errno));
      return false;
    }
  }
  if (grcore_group_create(NULL, NULL, &server->group) != GRCORE_OK || !build_library(server)) {
    return false;
  }
  return true;
}

static void server_close(Server * server) {
  if (server->listener >= 0) {
    close(server->listener);
  }
  if (server->debug_listener >= 0) {
    close(server->debug_listener);
  }
  gltang_library_release(server->library);
  if (server->group) {
    grcore_group_destroy(server->group);
  }
}

/* ---- the self-test: a client on other threads ---------------------------- */

typedef struct {
  int status;
  char * head; /* the header block */
  char * body;
} HttpReply;

static int connect_loopback(int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  CHECK(fd >= 0);
  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons((uint16_t)port);
  CHECK(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
  return fd;
}

/** Sends `raw` and reads the reply to the end of the stream, each wait bounded. */
static HttpReply http_raw(int port, const char * raw) {
  HttpReply reply = {0, NULL, NULL};
  int fd = connect_loopback(port);
  CHECK(send_all(fd, raw, strlen(raw)));
  Buf in = {NULL, 0, 0};
  long deadline = now_ms() + 30000;
  for (;;) {
    long left = deadline - now_ms();
    CHECK(left > 0);
    if (!wait_readable(fd, (int)left)) {
      CHECK(!"the server did not answer in time");
    }
    char chunk[4096];
    ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
    if (n <= 0) {
      break;
    }
    CHECK(buf_append(&in, chunk, (size_t)n));
  }
  close(fd);
  CHECK(in.data != NULL);
  CHECK(sscanf(in.data, "HTTP/1.1 %d", &reply.status) == 1);
  char * split = strstr(in.data, "\r\n\r\n");
  CHECK(split != NULL);
  reply.head = strndup(in.data, (size_t)(split - in.data));
  reply.body = strdup(split + 4);
  free(in.data);
  return reply;
}

static HttpReply http_get(int port, const char * target) {
  char raw[512];
  snprintf(raw, sizeof(raw), "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n", target);
  return http_raw(port, raw);
}

static void reply_free(HttpReply * reply) {
  free(reply->head);
  free(reply->body);
}

/** The value of a response header, as a number, or -1. */
static long header_number(const HttpReply * reply, const char * name) {
  char needle[64];
  snprintf(needle, sizeof(needle), "\r\n%s: ", name);
  const char * at = strstr(reply->head, needle);
  return at ? strtol(at + strlen(needle), NULL, 10) : -1;
}

/* A DAP client: hand-framed, as an editor's would be. */
typedef struct {
  int fd;
  Buf pending;
  int seq;
} Dap;

static bool dap_message(Dap * dap, char ** body) {
  long deadline = now_ms() + 20000;
  for (;;) {
    char * split = dap->pending.data ? strstr(dap->pending.data, "\r\n\r\n") : NULL;
    if (split) {
      size_t length = (size_t)strtoul(dap->pending.data + strlen("Content-Length: "), NULL, 10);
      size_t header = (size_t)(split - dap->pending.data) + 4;
      if (dap->pending.length >= header + length) {
        *body = strndup(dap->pending.data + header, length);
        memmove(dap->pending.data, dap->pending.data + header + length, dap->pending.length - header - length);
        dap->pending.length -= header + length;
        dap->pending.data[dap->pending.length] = '\0';
        return true;
      }
    }
    long left = deadline - now_ms();
    if (left <= 0 || !wait_readable(dap->fd, (int)left)) {
      return false;
    }
    char chunk[4096];
    ssize_t n = recv(dap->fd, chunk, sizeof(chunk), 0);
    if (n <= 0) {
      return false;
    }
    CHECK(buf_append(&dap->pending, chunk, (size_t)n));
  }
}

/** Sends a request and returns its response; the messages before it are dropped. */
static char * dap_request(Dap * dap, const char * command, const char * arguments) {
  char body[1024];
  int seq = ++dap->seq;
  int n = snprintf(body, sizeof(body), "{\"seq\":%d,\"type\":\"request\",\"command\":\"%s\",\"arguments\":%s}", seq, command, arguments);
  char header[64];
  int h = snprintf(header, sizeof(header), "Content-Length: %d\r\n\r\n", n);
  CHECK(send_all(dap->fd, header, (size_t)h) && send_all(dap->fd, body, (size_t)n));
  char needle[40];
  snprintf(needle, sizeof(needle), "\"request_seq\":%d,", seq);
  for (;;) {
    char * message = NULL;
    CHECK(dap_message(dap, &message));
    if (strstr(message, needle)) {
      return message;
    }
    free(message);
  }
}

/** Reads until a message containing `needle` (an event), and returns it. */
static char * dap_await(Dap * dap, const char * needle) {
  for (;;) {
    char * message = NULL;
    CHECK(dap_message(dap, &message));
    if (strstr(message, needle)) {
      return message;
    }
    free(message);
  }
}

static long json_number(const char * text, const char * key) {
  const char * at = strstr(text, key);
  return at ? strtol(at + strlen(key), NULL, 10) : -1;
}

static int line_of(const char * path, const char * needle) {
  FILE * file = fopen(path, "rb");
  CHECK(file != NULL);
  char line[1024];
  int number = 0;
  while (fgets(line, sizeof(line), file)) {
    ++number;
    if (strstr(line, needle)) {
      fclose(file);
      return number;
    }
  }
  fclose(file);
  return -1;
}

typedef struct {
  int port;
  const char * target;
  HttpReply reply;
} HttpJob;

static void * http_job_main(void * argument) {
  HttpJob * job = argument;
  job->reply = http_get(job->port, job->target);
  return NULL;
}

typedef struct {
  Server * server;
  int failures;
  int checks;
} SelfTest;

static void expect(SelfTest * test, bool condition, const char * format, ...) __attribute__((format(printf, 3, 4)));
static void expect(SelfTest * test, bool condition, const char * format, ...) {
  va_list args;
  va_start(args, format);
  printf("  %s ", condition ? "ok  " : "FAIL");
  vprintf(format, args);
  printf("\n");
  va_end(args);
  fflush(stdout);
  test->checks++;
  if (!condition) {
    test->failures++;
  }
}

/** Opens the debug connection, sends the opening of a session, and returns the client. */
static Dap dap_connect_and_configure(SelfTest * test, const char * page_path, int line) {
  Dap dap = {connect_loopback(test->server->debug_port), {NULL, 0, 0}, 0};
  char * reply = dap_request(&dap, "initialize", "{\"adapterID\":\"tang\",\"linesStartAt1\":true}");
  free(reply);
  reply = dap_request(&dap, "launch", "{}");
  free(reply);
  char arguments[1200];
  snprintf(arguments, sizeof(arguments), "{\"source\":{\"path\":\"%s\"},\"breakpoints\":[{\"line\":%d}]}", page_path, line);
  reply = dap_request(&dap, "setBreakpoints", arguments);
  expect(test, strstr(reply, "\"verified\":true") != NULL, "setBreakpoints %s:%d verified", page_path, line);
  free(reply);
  reply = dap_request(&dap, "configurationDone", "{}");
  free(reply);
  return dap;
}

static void dap_close(Dap * dap) {
  close(dap->fd);
  free(dap->pending.data);
}

static void self_test_debug(SelfTest * test, const char * page_path, int side_line, const HttpReply * plain) {
  Server * server = test->server;
  char target[128];

  /* 1. A full session: breakpoint, locals, step in, over, out, continue. */
  HttpJob job = {server->port, "/page?debug=1", {0, NULL, NULL}};
  pthread_t thread;
  CHECK(pthread_create(&thread, NULL, http_job_main, &job) == 0);
  Dap dap = dap_connect_and_configure(test, page_path, side_line);
  char * stopped = dap_await(&dap, "\"event\":\"stopped\"");
  expect(test, strstr(stopped, "\"reason\":\"breakpoint\"") != NULL, "stopped at the breakpoint (reason breakpoint)");
  free(stopped);
  char * trace = dap_request(&dap, "stackTrace", "{\"threadId\":1}");
  expect(test, strstr(trace, page_path) != NULL && json_number(trace, "\"line\":") == side_line && json_number(trace, "\"totalFrames\":") == 1,
      "stackTrace: %s:%d, one frame", page_path, side_line);
  long frame = json_number(trace, "\"id\":");
  free(trace);
  char arguments[96];
  snprintf(arguments, sizeof(arguments), "{\"frameId\":%ld}", frame);
  char * scopes = dap_request(&dap, "scopes", arguments);
  long reference = json_number(scopes, "\"variablesReference\":");
  expect(test, strstr(scopes, "\"name\":\"program\"") != NULL, "scopes: the program's scope");
  free(scopes);
  snprintf(arguments, sizeof(arguments), "{\"variablesReference\":%ld}", reference);
  char * variables = dap_request(&dap, "variables", arguments);
  expect(test, strstr(variables, "\"name\":\"count\",\"value\":\"3\"") != NULL && strstr(variables, "\"name\":\"items\"") != NULL,
      "variables: count is 3 and items is there");
  free(variables);
  char * step = dap_request(&dap, "stepIn", "{\"threadId\":1}");
  free(step);
  stopped = dap_await(&dap, "\"event\":\"stopped\"");
  free(stopped);
  trace = dap_request(&dap, "stackTrace", "{\"threadId\":1}");
  expect(test, json_number(trace, "\"totalFrames\":") == 2 && strstr(trace, "sidebar.tang") != NULL, "stepIn: two frames deep, in sidebar.tang");
  free(trace);
  step = dap_request(&dap, "next", "{\"threadId\":1}");
  free(step);
  stopped = dap_await(&dap, "\"event\":\"stopped\"");
  free(stopped);
  trace = dap_request(&dap, "stackTrace", "{\"threadId\":1}");
  expect(test, json_number(trace, "\"totalFrames\":") == 2, "next: still two frames deep");
  free(trace);
  step = dap_request(&dap, "stepOut", "{\"threadId\":1}");
  free(step);
  stopped = dap_await(&dap, "\"event\":\"stopped\"");
  free(stopped);
  trace = dap_request(&dap, "stackTrace", "{\"threadId\":1}");
  expect(test, json_number(trace, "\"totalFrames\":") == 1 && strstr(trace, page_path) != NULL, "stepOut: back in page.tang, one frame");
  free(trace);
  step = dap_request(&dap, "continue", "{\"threadId\":1}");
  free(step);
  char * ended = dap_await(&dap, "\"event\":\"terminated\"");
  expect(test, ended != NULL, "continue: the run ends with terminated");
  free(ended);
  char * bye = dap_request(&dap, "disconnect", "{}");
  free(bye);
  dap_close(&dap);
  CHECK(pthread_join(thread, NULL) == 0);
  expect(test, job.reply.status == 200 && strcmp(job.reply.body, plain->body) == 0, "the debugged /page answers 200 with the undebugged body (%zu bytes)", strlen(job.reply.body));
  reply_free(&job.reply);

  /* 2. A client that closes the connection at a stop: the request finishes free. */
  job.reply = (HttpReply){0, NULL, NULL};
  CHECK(pthread_create(&thread, NULL, http_job_main, &job) == 0);
  dap = dap_connect_and_configure(test, page_path, side_line);
  stopped = dap_await(&dap, "\"event\":\"stopped\"");
  free(stopped);
  dap_close(&dap);
  CHECK(pthread_join(thread, NULL) == 0);
  expect(test, job.reply.status == 200 && strcmp(job.reply.body, plain->body) == 0, "a client that closes mid-stop: debugger disarmed, the request finishes (200, same body)");
  reply_free(&job.reply);

  /* 3. A client that terminates: 503. */
  job.reply = (HttpReply){0, NULL, NULL};
  CHECK(pthread_create(&thread, NULL, http_job_main, &job) == 0);
  dap = dap_connect_and_configure(test, page_path, side_line);
  stopped = dap_await(&dap, "\"event\":\"stopped\"");
  free(stopped);
  char * terminated = dap_request(&dap, "terminate", "{}");
  free(terminated);
  ended = dap_await(&dap, "\"event\":\"terminated\"");
  free(ended);
  dap_close(&dap);
  CHECK(pthread_join(thread, NULL) == 0);
  expect(test, job.reply.status == 503 && strstr(job.reply.body, "terminated by the debugger") != NULL, "terminate: 503 'terminated by the debugger'");
  reply_free(&job.reply);

  /* 4. No debugger connects in time: 504. */
  snprintf(target, sizeof(target), "/page?debug=1");
  HttpReply late = http_get(server->port, target);
  expect(test, late.status == 504 && strstr(late.body, "no debugger connected") != NULL, "no debugger within %d ms: 504", server->config.debug_wait_ms);
  reply_free(&late);
}

static void * self_test_main(void * argument) {
  SelfTest * test = argument;
  Server * server = test->server;
  char path[1024];
  printf("self-test: server on 127.0.0.1:%d, debugger on 127.0.0.1:%d, templates in %s\n", server->port, server->debug_port, server->config.templates);

  HttpReply page = http_get(server->port, "/page");
  expect(test, page.status == 200 && strstr(page.body, "<h1>Page</h1>") != NULL && strstr(page.body, "3 items") != NULL
      && strstr(page.body, "<li>docs</li>") != NULL && strstr(page.body, "<header>Ghoti</header>") != NULL,
      "/page answers 200 with the page, the sidebar and the layout (%zu bytes)", strlen(page.body));
  HttpReply again = http_get(server->port, "/page");
  expect(test, again.status == 200 && strcmp(again.body, page.body) == 0, "a second /page answers the same body");
  expect(test, header_number(&page, "X-Context-Id") > 0 && header_number(&again, "X-Context-Id") > header_number(&page, "X-Context-Id"),
      "each request had a context of its own (X-Context-Id %ld, then %ld)", header_number(&page, "X-Context-Id"), header_number(&again, "X-Context-Id"));
  expect(test, header_number(&page, "X-Template-Errors") == 0, "/page has an empty error list (X-Template-Errors: 0)");
  reply_free(&again);

  HttpReply slow = http_get(server->port, "/slow");
  expect(test, slow.status == 200 && header_number(&slow, "X-Fuel-Raised") == 1 && strstr(slow.body, "done:") != NULL,
      "/slow answers 200 after one raise of the budget");
  reply_free(&slow);

  template_path(server, "runaway", path, sizeof(path));
  int loop_line = line_of(path, "while (true)");
  HttpReply runaway = http_get(server->port, "/runaway");
  char expected[1200];
  snprintf(expected, sizeof(expected), "paused at %s:%d", path, loop_line);
  expect(test, runaway.status == 503 && strncmp(runaway.body, expected, strlen(expected)) == 0, "/runaway answers 503 '%s'", expected);
  reply_free(&runaway);
  HttpReply survivor = http_get(server->port, "/page");
  expect(test, survivor.status == 200 && strcmp(survivor.body, page.body) == 0, "after the runaway, /page still answers 200 (the server survived)");
  reply_free(&survivor);

  HttpReply broken = http_get(server->port, "/broken");
  expect(test, broken.status == 200 && header_number(&broken, "X-Template-Errors") == 1, "/broken answers 200 with X-Template-Errors: 1");
  reply_free(&broken);

  HttpReply refused = http_get(server->port, "/refused");
  template_path(server, "refused", path, sizeof(path));
  expect(test, refused.status == 500 && strncmp(refused.body, path, strlen(path)) == 0, "/refused answers 500 with '%s:line:col: message'", path);
  reply_free(&refused);

  HttpReply garbage = http_raw(server->port, "this is not http\r\n\r\n");
  expect(test, garbage.status == 400, "a malformed request answers 400");
  reply_free(&garbage);
  HttpReply post = http_raw(server->port, "POST /page HTTP/1.1\r\nContent-Length: 0\r\n\r\n");
  expect(test, post.status == 400, "a method other than GET answers 400");
  reply_free(&post);
  HttpReply unknown = http_get(server->port, "/no-such-template");
  expect(test, unknown.status == 404, "an unknown template answers 404");
  reply_free(&unknown);
  HttpReply traversal = http_get(server->port, "/../etc/passwd");
  expect(test, traversal.status == 404, "a name with '..' and '/' answers 404");
  reply_free(&traversal);
  HttpReply nested = http_get(server->port, "/a/b");
  expect(test, nested.status == 404, "a name with '/' answers 404");
  reply_free(&nested);
  HttpReply upper = http_get(server->port, "/Page");
  expect(test, upper.status == 404, "a name outside [a-z0-9_-] answers 404");
  reply_free(&upper);

  template_path(server, "page", path, sizeof(path));
  int side_line = line_of(path, "side = sidebar();");
  CHECK(side_line > 0);
  self_test_debug(test, path, side_line, &page);
  reply_free(&page);

  HttpReply last = http_get(server->port, "/page");
  expect(test, last.status == 200, "and the server still answers after all of it");
  reply_free(&last);

  printf("self-test: %d checks, %d failed\n", test->checks, test->failures);
  fflush(stdout);
  atomic_store(&g_stop, 1);
  return NULL;
}

static int run_self_test(Server * server) {
  /* The backstop: no wait here is unbounded, and this makes sure of it. */
  alarm(180);
  SelfTest test = {server, 0, 0};
  pthread_t thread;
  CHECK(pthread_create(&thread, NULL, self_test_main, &test) == 0);
  serve_loop(server);
  CHECK(pthread_join(thread, NULL) == 0);
  alarm(0);
  return test.failures == 0 && test.checks > 0 ? 0 : 1;
}

/* ---- main --------------------------------------------------------------- */

static void usage(void) {
  printf(
      "Usage: web_server [OPTIONS]\n"
      "\n"
      "Serve the templates in a directory over HTTP on 127.0.0.1, one context per request.\n"
      "\n"
      "  --port N            Listen on this port (default 0: an ephemeral one, printed)\n"
      "  --templates DIR     The directory of NAME.tang files (default examples/web)\n"
      "  --fuel N            The fuel budget of a request (default %u)\n"
      "  --raise N           What a first pause raises it by (default ten times the budget)\n"
      "  --debug-port N      Also listen here for a debugger (0: ephemeral, printed); a\n"
      "                      request for /NAME?debug=1 then runs under the debugger\n"
      "  --debug-wait-ms N   How long such a request waits for a debugger (default %d)\n"
      "  --self-test         Play a client against the server, check it, and exit\n"
      "  --help              This text\n",
      DEFAULT_FUEL, DEFAULT_DEBUG_WAIT_MS);
}

static bool parse_number(const char * text, uint64_t * out) {
  char * end = NULL;
  errno = 0;
  unsigned long long value = strtoull(text, &end, 10);
  if (errno || *text == '\0' || *end != '\0' || *text == '-') {
    return false;
  }
  *out = (uint64_t)value;
  return true;
}

int main(int argc, char ** argv) {
  Config config;
  memset(&config, 0, sizeof(config));
  config.templates = "examples/web";
  config.fuel = DEFAULT_FUEL;
  config.debug_wait_ms = DEFAULT_DEBUG_WAIT_MS;
  for (int i = 1; i < argc; ++i) {
    uint64_t number = 0;
    bool has_value = i + 1 < argc;
    if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
      usage();
      return 0;
    }
    else if (!strcmp(argv[i], "--self-test")) {
      config.self_test = true;
    }
    else if (!strcmp(argv[i], "--templates") && has_value) {
      config.templates = argv[++i];
    }
    else if (!strcmp(argv[i], "--port") && has_value && parse_number(argv[i + 1], &number) && number <= 65535u) {
      config.port = (int)number;
      ++i;
    }
    else if (!strcmp(argv[i], "--debug-port") && has_value && parse_number(argv[i + 1], &number) && number <= 65535u) {
      config.debug_port = (int)number;
      config.has_debug = true;
      ++i;
    }
    else if (!strcmp(argv[i], "--fuel") && has_value && parse_number(argv[i + 1], &number)) {
      config.fuel = number;
      ++i;
    }
    else if (!strcmp(argv[i], "--raise") && has_value && parse_number(argv[i + 1], &number)) {
      config.raise = number;
      config.has_raise = true;
      ++i;
    }
    else if (!strcmp(argv[i], "--debug-wait-ms") && has_value && parse_number(argv[i + 1], &number) && number <= 600000u) {
      config.debug_wait_ms = (int)number;
      ++i;
    }
    else {
      fprintf(stderr, "web_server: bad or incomplete option %s (--help lists them)\n", argv[i]);
      return 2;
    }
  }
  if (!config.has_raise) {
    config.raise = config.fuel * 10u;
  }
  if (config.self_test) {
    config.has_debug = true;
    config.debug_port = 0;
    config.port = 0;
    /* Short enough that the "nobody connected" case does not make a build wait. */
    config.debug_wait_ms = 2000;
  }

  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_handler = on_signal;
  sigemptyset(&action.sa_mask);
  sigaction(SIGINT, &action, NULL);
  sigaction(SIGTERM, &action, NULL);
  signal(SIGPIPE, SIG_IGN);

  Server server;
  memset(&server, 0, sizeof(server));
  server.config = config;
  server.listener = -1;
  server.debug_listener = -1;
  int status = 1;
  if (server_open(&server)) {
    if (!config.self_test) {
      printf("web_server: serving %s on http://127.0.0.1:%d/ (try /page)\n", config.templates, server.port);
      if (server.debug_listener >= 0) {
        printf("web_server: debugger on 127.0.0.1:%d (request /NAME?debug=1, then connect a DAP client)\n", server.debug_port);
      }
      fflush(stdout);
      serve_loop(&server);
      printf("web_server: stopped\n");
      status = 0;
    }
    else {
      status = run_self_test(&server);
    }
  }
  server_close(&server);
  return status;
}
