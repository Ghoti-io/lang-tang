# Examples

Each program here is built and run by `make examples` (and by `make test`), so
an example that stops working fails the build. Run them with the same
`PREFIX=` the library was built with. They are C. `parse_template.c` needs only
the umbrella header, `<ghoti.io/lang-tang/lang-tang.h>`; the two that run a
program also include the free headers for compiling and executing
(`compile.h`, `program.h`, `execution.h`, `value.h`) and runtime-core's and
runtime-heap's umbrella headers. The ones that give a program libraries and
templates also include `library.h`, and `host_values.c` `description.h`.

| To see how to... | Read |
| --- | --- |
| Parse a template into a tree and count its nodes | [`parse_template.c`](parse_template.c), `main` |
| Parse a script instead of a template | [`parse_template.c`](parse_template.c), the second `gltang_parse` call (`GLTANG_PARSE_SCRIPT`) |
| Find out where and why a source was refused (`GLTANG_ParseError`) | [`parse_template.c`](parse_template.c), the second `gltang_parse` call |
| Release a tree | [`parse_template.c`](parse_template.c), `gltang_tree_destroy` |
| Compile a tree to a program and say where it was refused | [`run_template.c`](run_template.c), the `gltang_compile` call |
| Build the runtime a program needs (group, context, heap, execution) | [`run_template.c`](run_template.c), after the compile |
| Run a template and read the output, raw and rendered per segment | [`run_template.c`](run_template.c), `gltang_execution_output_raw` and `_render` |
| Stop a runaway loop and find the file and line it stopped on | [`pause_resume.c`](pause_resume.c), `grcore_context_pause_location` |
| Raise the budget and resume, or give up and unwind | [`pause_resume.c`](pause_resume.c), `grcore_context_set_fuel`, `grcore_context_terminate` |
| Give a program data and functions (a `user` library, a native function) | [`inject_context.c`](inject_context.c), `render_for`, the `gltang_library_add_*` calls |
| Run one compiled template in many contexts, each with its own user | [`inject_context.c`](inject_context.c), `main` and `render_for` |
| Write a native function: read its arguments, answer with a value or an error | [`inject_context.c`](inject_context.c), `greet` |
| Answer with a string that is escaped on output (an encoding tag) | [`inject_context.c`](inject_context.c), `greet`, `gltang_call_return_string` |
| Hand a template an array of maps (a menu) that the host builds without touching the program's heap | [`host_values.c`](host_values.c), `build_menu`: `gltang_value_description_create`, `gltang_value_array`, `gltang_value_map`, `gltang_value_string` |
| Share one built value between many executions, each of which gets its own copy | [`host_values.c`](host_values.c), `main` and `render`: `gltang_library_add_value` |
| Build a value only when a program reaches it | [`host_values.c`](host_values.c), `stats_factory`: `gltang_library_add_value_factory` |
| Answer a native call with an array or a map, built during the call and charged to the run | [`host_values.c`](host_values.c), `search`: `gltang_call_description`, `gltang_call_return_value` |
| Keep an encoding (escaped, percent-encoded) on a string inside a container | [`host_values.c`](host_values.c), the `GLTANG_String_Type` of each `gltang_value_string` and `gltang_value_map_set` |
| Read an array of maps in a native, keeping each string's encoding | [`handles.c`](handles.c), `titles`: `gltang_call_arg`, `gltang_handle_array_get`, `gltang_handle_map_get` |
| Keep the handles of a long loop few | [`handles.c`](handles.c), `titles`: `gltang_handle_push_frame` and `_pop_frame` |
| Give an object a method that knows which object it is on | [`handles.c`](handles.c), `account` and `label`: `gltang_handle_make_native`, `gltang_call_bound` |
| Answer a library from a factory (`use pages.recent as r`) | [`handles.c`](handles.c), `recent_factory`: `gltang_library_add_handle_factory`, `gltang_handle_make_library` |
| Register a template and call it from another (`use sidebar; sidebar()`) | [`nested_templates.c`](nested_templates.c), `gltang_library_add_template` |
| Stop a runaway nav pane at its own boundary and keep the page | [`nested_templates.c`](nested_templates.c), the scope fuel and `GLTANG_SCOPE_EMPTY` |
| Read the error list: which template, the chain of calls above it, file and line | [`nested_templates.c`](nested_templates.c), `gltang_execution_error` and `_error_chain` |
| See a function tier up, and read what the JIT did (and run in a build without it) | [`jit_hot_loop.c`](jit_hot_loop.c), `run_once`: `gltang_execution_set_jit_threshold`, `gltang_execution_jit_stats`, `gltang_jit_built` |
| Find the hot loop of a program by sampling it (a timer, a profile of self and inclusive samples by file and line), on the interpreter and the JIT | [`profile_hot_loop.c`](profile_hot_loop.c), `profile_once`: `grcore_profiler_attach`, `grcore_profiler_timer_start`, `grcore_profiler_report` |
| Ask why a value of a paused program is still alive (the chain from the guest stack through an array to the value) | [`retention_leak.c`](retention_leak.c), `main`: `grheap_retention_path`, `grheap_retention_dump` |
| Read a paused frame's local variables by name (the scope interface, as a debugger does) | [`retention_leak.c`](retention_leak.c), `find_local` |
| Freeze a context that has run its prologue, and start many contexts from it instead of redoing the prologue | [`snapshot_start.c`](snapshot_start.c), `main`: `gltang_snapshot_take`, `gltang_snapshot_restore` |
| Check that a context started from a snapshot prints what a run from scratch prints | [`snapshot_start.c`](snapshot_start.c), the comparison in the request loop |
| Resume a restored context (a snapshot of a paused context leaves the destination paused) | [`snapshot_start.c`](snapshot_start.c), `grcore_resume` after the restore |
| Serve templates over HTTP, a fresh context for each request | [`web_server.c`](web_server.c), `run_request`; the templates are in [`web/`](web/) |
| Answer a runaway template with its file and line, and keep serving | [`web_server.c`](web_server.c), `run_request` (raise the budget once, then a second pause is final) |
| Compile a template with the path it was read from, so a breakpoint names the real file | [`web_server.c`](web_server.c), `load_program` |
| Turn on statement polls and attach a debugger to one request's context | [`web_server.c`](web_server.c), `run_request` and `debug_create` |
| Run the host loop of a debug session (serve, run, notify, serve, resume) | [`web_server.c`](web_server.c), `run_request`; it is `runtime-debug`'s `examples/dap_session.c` loop |
| Play a scripted DAP client against it | [`web_server.c`](web_server.c), `self_test_debug` |

`web_server.c` is a host of the debugger, so it links `runtime-debug` and `text`
(which no other example may: `check-edges`), and `make examples` runs it with
`--self-test`. Run it by hand with `web_server --debug-port 0` and open
`http://127.0.0.1:PORT/page`; a request for `/page?debug=1` waits for a DAP
client on the debug port. It is a demonstration, not a policy for debugging a
server over a network: it binds `127.0.0.1` only, reads no request body and
speaks no TLS.

The `tang` command is the same interface behind a command line (and, with
`--dap`, a debugger host: the Debug Adapter Protocol on stdin and stdout, the
run's output on stderr): `tang FILE`
parses a template and prints its tree, `tang -s FILE` parses a script, and
`tang -e SOURCE` takes the source from the argument. `tang --help` lists the
rest (`--seed N`, `--log-errors`, `--halt-on-error` and `--errors` are the host API's
switches: the generators' master seed, the error list's, and the run's end at
the first error). `tang` runs what it parses: `tang FILE` runs a template, `tang -s FILE`
a script, `tang -e SOURCE` a script from the argument (`-t` makes it a
template), and `--tree` prints the tree instead.
