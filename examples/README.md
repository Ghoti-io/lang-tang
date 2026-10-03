# Examples

Each program here is built and run by `make examples` (and by `make test`), so
an example that stops working fails the build. Run them with the same
`PREFIX=` the library was built with. They are C. `parse_template.c` needs only
the umbrella header, `<ghoti.io/lang-tang/lang-tang.h>`; the two that run a
program also include the free headers for compiling and executing
(`compile.h`, `program.h`, `execution.h`, `value.h`) and runtime-core's and
runtime-heap's umbrella headers. The two that give a program libraries and
templates also include `library.h`.

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
| Register a template and call it from another (`use sidebar; sidebar()`) | [`nested_templates.c`](nested_templates.c), `gltang_library_add_template` |
| Stop a runaway nav pane at its own boundary and keep the page | [`nested_templates.c`](nested_templates.c), the scope fuel and `GLTANG_SCOPE_EMPTY` |
| Read the error list: which template, the chain of calls above it, file and line | [`nested_templates.c`](nested_templates.c), `gltang_execution_error` and `_error_chain` |
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
