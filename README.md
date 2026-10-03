# Ghoti.io Lang-tang

The Tang engine of the Ghoti.io language runtime stack, in C. Tang is a
template language: text with `<% code %>` and `<%= expression %>` tags, and the
same grammar as a script. `lang-tang` runs ctang's documented language on the
new stack and replaces [ctang](../ctang), which stays frozen as its oracle until
every difference between the two is fixed or recorded in the divergence ledger.

Nothing is released. So far there is the front end (the parser, the scanner and
the syntax tree, ported from ctang as this library's own source), a compiler to
the library's own bytecode, a switch-dispatched interpreter whose calls live on
the runtime-core context's guest stack, a heap of values described to
runtime-heap, the interface that parses, compiles and runs a template or a
script under fuel, memory and call-depth budgets (a run can pause, resume or
unwind), the host API over it - libraries (`math`, `random` and the host's own,
native functions, lazy factories), an error list that records the errors a
program swallowed, template calls that each run under a budget scope of their
own, and a random generator per context seeded from a sequence the host owns -
the `tang` command, the divergence ledger, and the oracle that compares every
parse with ctang's. The execution differential against ctang and the debugger
come in later work.

## Example

```c
#include <ghoti.io/lang-tang/lang-tang.h>

#include <stdio.h>

int main(void) {
  GLTANG_Tree * tree;
  GLTANG_ParseError error;

  /* A template: text, a code tag and a print tag. */
  if (gltang_parse("Hello <%= name %>!", GLTANG_PARSE_TEMPLATE, &error, &tree) != GLTANG_OK) {
    return 1;
  }
  printf("%zu nodes\n", gltang_tree_node_count(tree));
  gltang_tree_destroy(tree);

  /* A refusal says where and why, and leaves nothing to free. */
  if (gltang_parse("x = ;", GLTANG_PARSE_SCRIPT, &error, &tree) == GLTANG_ERR_FORMAT) {
    printf("%d:%d: %s\n", error.line, error.column, error.message);
  }
  return 0;
}
```

Compile it against an installed copy with
`cc example.c $(pkg-config --cflags --libs ghoti.io-lang-tang-0)`.
[examples/parse_template.c](examples/parse_template.c) is a complete version;
[examples/run_template.c](examples/run_template.c) goes on to compile and run a
template and read its output, [examples/pause_resume.c](examples/pause_resume.c)
stops a runaway loop at its file and line,
[examples/inject_context.c](examples/inject_context.c) gives one compiled
template a different `user` library and a native function in each of two
contexts, and [examples/nested_templates.c](examples/nested_templates.c) calls a
sidebar from a page and a nav pane that never ends from the sidebar, stops the
nav pane at its own budget scope, and reads the error list.

## Building

`lang-tang` depends on `cutil`, `unicode`, `runtime-core` and `runtime-heap`,
found through pkg-config only, and
needs `bison` (3.8.2 or later) and `flex` to generate the parser and scanner.
Build the suite first from the workspace root (`./bootstrap.sh`), then:

```bash
export PKG_CONFIG_PATH="$PWD/.local/share/pkgconfig"
make -C libs/lang-tang test PREFIX="$PWD/.local"
```

`make test` also needs ctang installed (`ghoti.io-tang-0`): the oracle
differential is part of it, and it **fails, never skips,** without it.

Pass `PREFIX=` to every `make`, including a throwaway one: the rpath is added
only when it is set. `make help` lists the targets. The ones particular to
this library:

| Target | Does |
| --- | --- |
| `test` | build, `check-symbols`, `check-aliasing` (gcc only), `check-stamps`, the gates below, the examples, the unit tests (the engine suites again under the heap's torture mode and with a moving guest stack), the CLI test, the fuzz replay, the oracle differential, and one smoke run of the benchmark |
| `test-oracle` | parse every file of `tests/corpus` with lang-tang and with ctang (in a child process, with a wall-clock kill) and fail on any difference the ledger does not record; `ORACLE_PC` names the ctang package |
| `check-labels` | fail if a public header has no `@stability` label, or the wrong one (`stable` for the C interface, `free` for the syntax tree's node classes) |
| `check-edges` | fail on any `#include` or shared-object dependency on a Ghoti library other than `cutil`, `unicode`, `runtime-core`, `runtime-heap` and this one - ctang above all - and on any include of `binary.h` |
| `check-gates` | run each gate against a planted defect and a control, and against an empty tree, and fail unless each behaves |
| `cli-test` | run the `tang` command over its documented cases and exit statuses |
| `fuzz-replay` | feed every corpus and seed file once through the fuzz entry points, in an ordinary build |
| `fuzz-parse`, `fuzz-template`, `fuzz-run` | build the libFuzzer harnesses (clang); `fuzz-run-parse`, `fuzz-run-template` and `fuzz-run-run` run them |
| `bench` | run the benchmark harness in full; it prints a calibration result first |
| `test-asan`, `test-tsan`, `test-valgrind-quiet` | the same tests under ASan+UBSan, ThreadSanitizer and Valgrind |
| `test-torture` | the engine suites under ASan+UBSan with the heap collecting before every allocation (`GRHEAP_TORTURE`), verifying every store (`GRHEAP_VERIFY`), and a guest stack that moves on every push (`GLTANG_TEST_MOVING_STACK`) |
| `coverage` | instrumented run and line report |

## The API

All headers are in `include/ghoti.io/lang-tang/`. `lang-tang.h` includes the
stable ones.

| Header | Label | Holds |
| --- | --- | --- |
| `parse.h` | stable | `gltang_parse`, `GLTANG_Tree`, `GLTANG_ParseError`, `gltang_tree_destroy`, `gltang_tree_node_count`, `gltang_tree_root`, `gltang_tree_print` |
| `core.h` | stable | `GLTANG_Result` (the suite's vocabulary), `gltang_result_string`, the version |
| `allocator.h` | stable | `GLTANG_Allocator`, `gltang_allocator_default`, `gltang_allocator` (the one the library allocates through) |
| `ast/*.h`, `location.h`, `unicodeString.h` | free | the node classes and what they are built on; the compiler reads them, so their shape may change |
| `compile.h`, `program.h`, `bytecode.h` | free | `gltang_compile`, the immutable reference-counted `GLTANG_Program`, the opcode table |
| `execution.h`, `value.h` | free | `GLTANG_Execution` (a program on a runtime-core context), its entry point for `grcore_run`, the result, output and error-origin accessors, the heap codec, the setters (`set_libraries`, `set_seeds`, `set_name`, `set_log_all_errors`, `set_halt_on_error`, `set_error_limit`) and the error list (`gltang_execution_error*`) |
| `library.h` | free | `GLTANG_Library`: a sealed, reference-counted table of members (values, native functions, templates, sub-libraries, lazy factories) for `use`, and the call object a native function reads its arguments from |
| `seeds.h` | stable | `GLTANG_SeedSequence`: the master seed and atomic counter that every execution's `random.global` and `random.default` are seeded from |

**Who owns what.** A parse result owns its tree and `gltang_tree_destroy` frees
it. The root and every node are borrowed from the tree. Memory is cutil's
(`gcu_malloc`), exactly as ctang's was; a run's own allocations are the
context's, and the parse is not charged to it (design.md says why). A program is
immutable and may be run by many contexts on many threads at once. An output parameter is written only on success,
except the optional `GLTANG_ParseError`, which is written when and only when the
result is `GLTANG_ERR_FORMAT`.

**Threads.** A parse is independent of every other: nothing is shared between
calls. A paused context may be resumed on a different thread. A library is
sealed when it is attached and may then be read by any number of contexts on any
number of threads; a seed sequence may be drawn from by any thread. A native
function runs on the context's owner thread inside the run, cannot pause, and
cannot call back into the execution that is running it (design.md, "The callback
contract").

## The `tang` command

```bash
tang FILE                    # run a template and print its output
tang -s FILE                 # ... a script
tang -e 'print(1 + 2);'      # ... a script given on the command line (-t: a template)
echo 'print(1 + 2);' | tang -s   # ... or from stdin
tang --tree FILE             # print the tree instead of running
tang --fuel 10000 -e 'while (true) {}'   # a budget; the pause names file and line
tang --seed 5 -e 'use random; print(random.global.next_int);'  # a fixed master seed
tang --errors -e 'print(1 / 0);'         # the error list, to stderr: main:<evaluate>:1: Divide by zero
tang --halt-on-error -e 'print("a"); print(1 / 0); print("b");'   # prints a, exit status 8
```

A syntax or compile error is `name:line:column: message` on stderr and exit
status 1; usage errors exit 2, a file that cannot be read 3, out of memory 4, a
run paused for fuel 5 (at a poll), a run unwound 6 (a limit reached
inside one operation, such as a huge repeat or copy, cannot pause), the runtime
could not be set up for a reason other than memory 7, and a run ended by
`--halt-on-error` 8. `--log-errors` enters every error in the error list when it
is created and not only the ones the program swallowed; `--errors` writes the list
after the run, one `template:file:line: message` an entry with the chain of
template calls above it indented under it.

## Documentation

- [documentation/design.md](documentation/design.md): what was ported from
  ctang and what was changed, and the alternatives it rejected.
- [documentation/divergence-ledger.md](documentation/divergence-ledger.md): every
  way lang-tang may differ from ctang, and the condition that retires ctang.
- [examples/README.md](examples/README.md) indexes the runnable examples by
  task.

## Status

Version 0.0.0, unreleased. Patches are not being accepted at this time; see
[CONTRIBUTING.md](CONTRIBUTING.md).
