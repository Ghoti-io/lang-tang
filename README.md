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
the `tang` command, the divergence ledger, and the oracle that parses and runs
every corpus file and 12,000 generated programs on both engines and compares the
output and the result with ctang's. The verification suite is also shown to fail:
a frame observer compares the abstract frames of one program run plain, under GC
torture, on a moving stack and with shuffled poll phases; a native gate drives
each guest-driven native and each limit under a tiny budget; and planted defects
in the library itself are each caught by the instrument named for them. The
debugger is [runtime-debug](../runtime-debug)'s, and two programs here are its
hosts: `tang --dap` speaks the Debug Adapter Protocol on stdin and stdout, and
[examples/web_server.c](examples/web_server.c) serves templates one context per
request, answers a runaway template with `503` and the file and line it was
stopped on, and lets the same template be stepped over DAP. [editors/vscode](editors/vscode) is a minimal VS Code extension that starts `tang --dap` as the debug adapter. The engine also has
a **baseline JIT** behind a build option (`JIT=yes`, the default): a function that
is hot is compiled, through [runtime-jit](../runtime-jit), to machine code for
small-integer and boolean work, entered right after its entry poll, and left for
the interpreter, in the same frame, at the first guard that fails or the first
operation it does not compile. A call of a declared function from compiled code is
a call between compiled functions, through the callee's entry slot and behind a
guard on the callee value, with the chain of compiled frames described to the
collector, the debugger and the frame differential and rebuilt into its guest
frames on a pause, a guard or a budget (a context with no native-stack byte budget
compiles no call; `tang --native-stack BYTES`, 1 MiB by default, sets it), and a
call of a library native and the load of a library member (`use`, `.name`) with no
exit, through the one wrapper the interpreter's `CALL` also enters a native by (which
opens the native's activation record and draws on the native-depth budget alike in both
tiers, so the native-depth budget now also limits interpreted programs: a native call nested deeper than the budget is refused with the recursion-limit error, in the interpreter as in compiled code). The output, the errors, the fuel and the polls are the interpreter's, which a frame
differential (`tests/observer.h`, interpreter against JIT, at every poll, chains
included), a fuel-parity test and a scripted debugger session with a breakpoint in a
compiled callee each check. Compiled code is not charged to the guest's memory
budget (its pages and records are the engine's, counted in the JIT statistics), so a
budget gives the same verdict compiled and interpreted, and a pause never throws a
hot function's code away, only guard exits do; `JIT=no` builds the
interpreter-only engine and links nothing of it. A paused or new execution can be
frozen into a **snapshot** and restored into a fresh context on any thread to
finish exactly as an uninterrupted run does
([examples/snapshot_start.c](examples/snapshot_start.c)): the snapshot holds no
address, so the library, native function and template values the program holds
are found again by name, and starting a context from a snapshot that has run a
heavy prologue is an order of magnitude cheaper than running the prologue
(`documentation/design.md`, "Snapshots"). Two questions a developer asks of a
running program are answered by services of the runtime stack: where the time
goes (`runtime-core`'s sampling profiler, [examples/profile_hot_loop.c](examples/profile_hot_loop.c))
and why a value is still alive (`runtime-heap`'s retention query,
[examples/retention_leak.c](examples/retention_leak.c)); the engine needed
nothing for either, since a poll identity is the same on every tier and its
values are described to the collector. The library itself
does not depend on the debugger; it only polls where a host may stop it.

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
nav pane at its own budget scope, and reads the error list, and
[examples/web_server.c](examples/web_server.c) is a web server over templates
(`examples/web/`) with one context per request, a fuel budget that pauses a
runaway at its file and line, and a debugger socket: `make examples` runs it
with `--self-test`, which plays a client, including a scripted DAP session.

## Building

`lang-tang` depends on `cutil`, `unicode`, `runtime-core` and `runtime-heap`,
and, unless built with `JIT=no`, on `runtime-jit` (a hard error naming the fix if
it cannot be found: `JIT=yes` is the default), found through pkg-config only.
`make JIT=no` builds the interpreter-only engine in a tree of its own
(`build/<os>/release-nojit`), compiles none of `src/jit/`, includes and links
nothing of `runtime-jit` (`check-edges` proves it for both arms), and reports the
JIT as absent (`gltang_jit_built()` is false, `gltang_execution_set_jit_threshold`
is `GLTANG_ERR_UNSUPPORTED`); `make test` runs that arm as well. Any other value
of `JIT` is a hard error. The `tang` command and the web-server example
also need `runtime-debug` (and `text`, which it requires): they are the library's
two hosts of the debugger, and the shared and static library link neither
(`check-edges` enforces it). `make WITH_DEBUG=no` builds the shared and static
library (`make all`) and any one unit-test binary (for example
`make build/linux/release/apps/testEngine`) without them; the two programs then
refuse to build, by name, rather than build without `--dap`. `make test` is not
available that way: its gates (`check-edges`, `examples`, `cli-test`) need the two
programs. It also
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
| `test` | build, `check-symbols`, `check-aliasing` (gcc only), `check-stamps`, the gates below, the examples, the unit tests (the engine suites, the frame observer and the native gate again under the heap's torture mode and with a moving guest stack, and, with the JIT built, again with every function compiled at its first poll), the CLI test, the fuzz replay, the oracle differential and its fixed batch of generated programs, the quick planted defects, one smoke run of the benchmark, and the `JIT=no` arm (`test-nojit`); with `RELOCATE_PREFIX` named, the relocation arm (`test-relocate`) too, and without it a loud line saying that arm was not run |
| `test-nojit` | the library built with `JIT=no` in its own tree, running the whole unit suite, the CLI test, the examples and the gates that apply, with the JIT-only tests compiled out |
| `test-nodebug` | the library and its unit suites built with `WITH_DEBUG=no` in a tree of its own, against a copy of `PREFIX` from which `runtime-debug` and `text` are taken away, with the gates that need no host (`PREFIX` is required); `test_tang_dap`, which runs the `tang` command, is the one suite left out |
| `test-relocate` | the relocation arm: every unit suite against a runtime-heap built with `RELOCATE=yes` (`RELOCATE_PREFIX` names the prefix it is installed in), in which every collection moves every unpinned object and poisons the old cell, interpreted and with the JIT at threshold 1, the engine suites again with torture and verify so that a move happens at every GC point, and the two planted relocation defects; it fails, never skips, when that heap has no relocation mode (`tools/check-relocation-required.sh`); with `RELOCATE_PREFIX` given, `test` runs it |
| `test-oracle` | parse and then run every file of `tests/corpus`, and 440 generated programs, with lang-tang and with ctang (in a child process, with a wall-clock kill and an address-space bound) and fail on any difference in the parse verdict, the rendered output or the final result that the ledger does not record; `ORACLE_PC` names the ctang package |
| `fuzz-diff` | `make fuzz-diff FUZZ_DIFF_COUNT=N FUZZ_DIFF_SEED=S`: a campaign of N generated programs from seed S, both modes; a divergence prints its seed and the whole program |
| `check-planted` | build a throwaway copy of the library, plant thirty defects one at a time (a missing root, a missing `gc_store`, an order-dependent DECIDE handler, a native that never polls, a wrong frame slot, a wrong operator, a silent oracle runner, the JIT's two: a wrong tag on a compiled `ADD` and a skipped fuel charge, the eleven of compiled calls: a push hook refused deep in a chain (so `fib(22)` makes call exits), a push hook that reads its arguments before the push, omits the `CALL`'s fuel or tests the depth one level early, a deopt hook that does not set the callers' identities or ignores a failed rebuild, a poll helper that copies the guest frame back, an uncompilable callee whose slot is not refused or whose exits count against its caller, a call that passes the entry flag as 1, and a discard that destroys the code directly, the six of library calls from compiled code: a native's record never left, a native-depth miscount, a native's arguments left unpinned, a stale pointer into the guest stack after a nested activation, an unwind status read as OK, and a compiled member load by name that is not charged, and the two of snapshots: a host pointer left in a type's payload with no hook, and a skipped output-buffer capture) and require the instrument named for each to fail and, with the patch out, to pass; `check-planted-quick` is part of `test`, `check-planted-slow` of `test-torture`, `check-planted-selftest` proves the script; cases 13, 14, 15, 20 and 27 (a reference visited and not updated, put back stale, or not pinned) need a heap that moves objects and are run by `check-planted-relocate`, which `test-relocate` calls |
| `check-labels` | fail if a public header has no `@stability` label, or the wrong one (`stable` for the C interface, `free` for the syntax tree's node classes) |
| `check-edges` | fail on any `#include` or shared-object dependency on a Ghoti library other than `cutil`, `unicode`, `runtime-core`, `runtime-heap` and this one (and `runtime-jit` under `JIT=yes`, in `src/jit/` only; under `JIT=no` nothing of it at all) - ctang above all - and on any include of `binary.h`; `runtime-debug` and `text` are allowed only in `src/tang.c` and `examples/web_server.c` (includes) and in the `tang` and `web_server` programs (NEEDED), never in the library |
| `check-gates` | run each gate against a planted defect and a control, and against an empty tree, and fail unless each behaves |
| `check-vscode` | check the VS Code debug contribution in `editors/vscode` (see its [README](editors/vscode/README.md)): a valid manifest that contributes a `tang` debugger and breakpoints in `.tang` files, and an extension that starts `tang` with `--dap`; `check-gates` runs it against a planted defect for each check |
| `check-backend-required` | run the tier-up tests as built and with the native backend forced off (`GLTANG_TEST_FORCE_NO_BACKEND=1`), and fail unless every one of them fails the second time: on Linux x86-64, Linux arm64 and Windows x86-64 a missing backend fails them, never skips them |
| `check-oracle-absent` | run `oracle-present` against a ctang package that does not exist and fail unless it exits non-zero, naming the package (and against the real one, which must pass) |
| `cli-test` | run the `tang` command over its documented cases and exit statuses, `--dap` included |
| `examples` | build and run every example; the web server with `--self-test` |
| `fuzz-replay` | feed every corpus and seed file once through the fuzz entry points, in an ordinary build |
| `fuzz-parse`, `fuzz-template`, `fuzz-run` | build the libFuzzer harnesses (clang); `fuzz-run-parse`, `fuzz-run-template` and `fuzz-run-run` run them |
| `bench` | run the benchmark harness in full; it prints a calibration result first |
| `test-asan`, `test-tsan`, `test-valgrind-quiet` | the same tests under ASan+UBSan, ThreadSanitizer and Valgrind |
| `test-torture` | every unit suite under ASan+UBSan with the heap collecting before every allocation (`GRHEAP_TORTURE`), verifying every store (`GRHEAP_VERIFY`), and a guest stack that moves on every push (`GLTANG_TEST_MOVING_STACK`), and the two torture planted defects |
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
| `execution.h`, `value.h` | free | `GLTANG_Execution` (a program on a runtime-core context), its entry point for `grcore_run`, the result, output and error-origin accessors, the heap codec, the setters (`set_libraries`, `set_seeds`, `set_name`, `set_log_all_errors`, `set_halt_on_error`, `set_statement_polls`, `set_error_limit`), the baseline JIT's host API (`gltang_execution_set_jit_threshold`, `gltang_execution_jit_stats`, `gltang_jit_built`) and the error list (`gltang_execution_error*`), and context snapshots (`gltang_snapshot_take`, `gltang_snapshot_restore`, `_retain`, `_release`, `_size`) |
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
tang --jit-threshold 1 --jit-stats -s hot.tang   # compile every function at its first poll; the JIT's counters go to stderr
tang --native-stack 4194304 -s deep.tang  # the native stack compiled code may use (default 1 MiB; 0: no limit, and then no call between compiled functions is compiled)
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
