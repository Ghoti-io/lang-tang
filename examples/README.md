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

The `tang` command is the same interface behind a command line: `tang FILE`
parses a template and prints its tree, `tang -s FILE` parses a script, and
`tang -e SOURCE` takes the source from the argument. `tang --help` lists the
rest (`--seed N`, `--log-errors`, `--halt-on-error` and `--errors` are the host API's
switches: the generators' master seed, the error list's, and the run's end at
the first error). `tang` runs what it parses: `tang FILE` runs a template, `tang -s FILE`
a script, `tang -e SOURCE` a script from the argument (`-t` makes it a
template), and `--tree` prints the tree instead.
