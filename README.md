# Ghoti.io Lang-tang

The Tang engine of the Ghoti.io language runtime stack, in C. Tang is a
template language: text with `<% code %>` and `<%= expression %>` tags, and the
same grammar as a script. `lang-tang` runs ctang's documented language on the
new stack and replaces [ctang](../ctang), which stays frozen as its oracle until
every difference between the two is fixed or recorded in the divergence ledger.

Nothing is released. So far there is the front end: the parser, the scanner and
the syntax tree, ported from ctang as this library's own source; the interface
that parses a template or a script into a tree the caller owns; the `tang`
command over it, which parses and dumps; the divergence ledger; and the oracle
that compares every parse with ctang's. The bytecode, the interpreter, the
budgets and the debugger come in later work, and so does anything that runs a
template.

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
[examples/parse_template.c](examples/parse_template.c) is a complete version.

## Building

`lang-tang` depends on `cutil` and `unicode`, found through pkg-config only, and
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
| `test` | build, `check-symbols`, `check-aliasing` (gcc only), `check-stamps`, the gates below, the examples, the unit tests, the CLI test, the fuzz replay, the oracle differential, and one smoke run of the benchmark |
| `test-oracle` | parse every file of `tests/corpus` with lang-tang and with ctang (in a child process, with a wall-clock kill) and fail on any difference the ledger does not record; `ORACLE_PC` names the ctang package |
| `check-labels` | fail if a public header has no `@stability` label, or the wrong one (`stable` for the C interface, `free` for the syntax tree's node classes) |
| `check-edges` | fail on any `#include` or shared-object dependency on a Ghoti library other than `cutil`, `unicode`, `runtime-core`, `runtime-heap` and this one - ctang above all - and on any include of `binary.h` |
| `check-gates` | run each gate against a planted defect and a control, and against an empty tree, and fail unless each behaves |
| `cli-test` | run the `tang` command over its documented cases and exit statuses |
| `fuzz-replay` | feed every corpus and seed file once through the fuzz entry points, in an ordinary build |
| `fuzz-parse`, `fuzz-template` | build the libFuzzer harnesses (clang); `fuzz-run-parse` and `fuzz-run-template` run them |
| `bench` | run the benchmark harness in full; it prints a calibration result first |
| `test-asan`, `test-tsan`, `test-valgrind-quiet` | the same tests under ASan+UBSan, ThreadSanitizer and Valgrind |
| `coverage` | instrumented run and line report |

## The API

All headers are in `include/ghoti.io/lang-tang/`. `lang-tang.h` includes the
stable ones.

| Header | Label | Holds |
| --- | --- | --- |
| `parse.h` | stable | `gltang_parse`, `GLTANG_Tree`, `GLTANG_ParseError`, `gltang_tree_destroy`, `gltang_tree_node_count`, `gltang_tree_root`, `gltang_tree_print` |
| `core.h` | stable | `GLTANG_Result` (the suite's vocabulary), `gltang_result_string`, the version |
| `allocator.h` | stable | `GLTANG_Allocator`, `gltang_allocator_default`, `gltang_allocator` (the one the library allocates through) |
| `ast/*.h`, `location.h`, `unicodeString.h` | free | the node classes and what they are built on; the compiler of a later story reads them, so their shape may change |

**Who owns what.** A parse result owns its tree and `gltang_tree_destroy` frees
it. The root and every node are borrowed from the tree. Memory is cutil's
(`gcu_malloc`), exactly as ctang's was; charging it to a runtime context is for
the story that adds execution. An output parameter is written only on success,
except the optional `GLTANG_ParseError`, which is written when and only when the
result is `GLTANG_ERR_FORMAT`.

**Threads.** A parse is independent of every other: nothing is shared between
calls.

## The `tang` command

```bash
tang FILE                # parse a template and print its tree
tang -s FILE             # ... a script
tang -e 'Hello <%= 1 %>' # ... the source from the argument
echo '1 + 2' | tang -s   # ... or from stdin
```

A syntax error is `name:line:column: message` on stderr and exit status 1;
usage errors exit 2, a file that cannot be read 3. Execution arrives with the
interpreter.

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
