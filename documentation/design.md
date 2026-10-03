# Design

**Status:** In progress. Describes what exists: the front end of the Tang
engine - the parser, the scanner and the syntax tree, ported from ctang as this
library's own source - the interface that parses a template or a script into
an owned tree, the `tang` command over it, the divergence ledger, and the
oracle that compares this library with frozen ctang. What is still not here is
listed at the end. The architecture it follows is the runtime stack's spine
(AD-2, AD-3, AD-9, AD-13, AD-14, AD-16, AD-26).

## What this library is

`lang-tang` is the Tang engine on the new runtime stack, and it replaces ctang.
The language is ctang's, unchanged: a template language whose code tags and
scripts share one grammar, with errors as values. What changes is everything
under the language - a new bytecode, a new interpreter, budgets, a collector,
a debugger - and none of that is here yet. This commit is the first piece,
and the one every later piece reads: **the tree.** The parser, the scanner and
the AST are ported from ctang; the interface is new; the oracle and the ledger
that keep the engine honest exist before any behaviour that could drift.

Nothing in this library is built on ctang's bytecode, interpreter, JIT or
`binary.h` (AD-9), and no name in it is ctang's: the prefix is `GLTANG`, never
`GTA`.

## Where the port came from

Ported from `libs/ctang` at commit

    f7562c69dacd670f0cd2132857ab27a1a0437278    Draw Tang's random values from cutil's engine

The oracle is the ctang installed from that same tree.

| ctang | lang-tang | What was kept |
| --- | --- | --- |
| `bison/tangParser.y` | `bison/tangParser.y` | The grammar, every action and every error path |
| `flex/tangScanner.l` | `flex/tangScanner.l` | The scanner: its states (script, template, quick print, comments, strings, dates) and every literal rule |
| `src/ast/*.c`, `include/.../ast/*.h` | `src/ast/*.c`, `include/.../ast/*.h` | 31 node classes, each with `create`, `destroy`, `print`, `walk`, and `astNode` the base |
| `src/tangLanguage.c` | `src/parse/parse.c` | Scanner setup, the parse, the rule that a failed parse is never an empty tree, `node_count` |
| `src/unicodeString.c`, `unicodeString.h` | the same names | The string type, `create`, `create_and_adopt`, `destroy` |
| `include/.../location.h`, `tangScanner.h` | the same names | The location type; the scanner's `YY_DECL` glue |
| `src/allocator.c`, `allocator.h` | `src/core/allocator.c`, `allocator.h` | The cutil-counting allocator, as `gltang_allocator()` |
| `src/tang.c` | `src/tang.c` | The command's flags; the rest is rewritten |
| `test/cli-test.sh` | `tests/cli-test.sh` | The shape of the CLI test |
| `test/fuzz/fuzz_parse.c`, `fuzz_template.c`, `lastInput.h`, seeds | `tests/fuzz/` | The two harnesses and the seeds |
| `test/test-tangLanguageParse.cpp` | `tests/unit/test_parse.cpp` | The parse-only tests; the ones that used `simplify` are not ported |

The names changed by one rule: `GTA_` became `GLTANG_`, `gta_` became `gltang_`,
`ghoti.io/tang/` became `ghoti.io/lang-tang/`. Bison's `api.prefix` and flex's
`prefix` changed with them (`GLTANG_Parser_`, `gltang_flex`).

**Deliberate changes**, every one of them:

- **Dropped, by the spec:** `compile_to_bytecode` and the four
  `compile_to_binary_*` slots, `simplify`, `analyze`, `possible_type`, and
  `GLTANG_Ast_Simplify_Variable_Map`; every reference to `Program`,
  `Compiler_Context`, `Variable_Scope`, `ComputedValue*`, `binary.h`,
  `bytecode.h` and the library registry. `simplify` is not ported at all: the
  milestone does not need constant folding to have a front end, and ctang
  itself had it switched off ("temporarily disabled because it does not pick up
  on global variable changes"). `gta_tang_node_count` is kept.
- **Fields that only analysis filled in** went with it: the mangled name and
  its hash, the scope pointer and the runtime-function pointer of a function
  node, the type, scope and mangled name of an identifier node. The compiler
  of a later story adds what it needs to headers labelled `free`.
- **The redeclaration parse-error singletons** (function, identifier, global)
  were analysis errors and went with it; the out-of-memory singleton stays.
- **`unicodeString`** keeps `create`, `create_and_adopt` and `destroy`.
  Concatenation, substring, rendering and HTML encoding serve execution; they
  and the four escape scanners (`htmlEscape`, `htmlEscapeAscii`,
  `percentEncode`, `unescape`) come with the interpreter. A stray file-scope
  object named `GCU_Type_Offset_Pair` and an exported helper named
  `gcu_unicode_string_get_grapheme_offsets` were not carried: the first was
  unused and the second was an exported symbol outside this library's prefix
  (it is `static` here).
- **`GLTANG_CALL`** (an `aapcs` attribute on ARM, empty on x86-64 and used by
  ctang's JIT) is gone. Only 64-bit targets are supported; ctang's 32-bit
  branch never defined the vector type its nodes use, so it could not have
  built.
- **Exported data** is declared with `GLTANG_API_DATA` (CONVENTIONS section 4),
  not `GLTANG_API`: ctang's spelling made every AST header fail to compile as
  C++ under `extern "C"`.
- **`GLTANG_NO_DISCARD`** is the GNU attribute in C and C++ alike, which is what
  makes the AST headers compile under clang++ (ctang reported this as a
  `[[nodiscard]]` placement defect).
- **The generated parser header** is `ghoti.io/lang-tang/ast/tangParser.h`,
  installed beside the AST headers that include it. ctang included it as
  `"tangParser.h"`, which no installed copy could resolve.
- **A parse-wide error location.** The parser takes one more parameter,
  `errorLocation`, so a rule that refuses its input (invalid UTF-8) can say
  where. ctang built the error node for such a refusal with no location.
- **Invalid UTF-8 is not "out of memory".** The same failed string creation
  covers both, and ctang reported both as `Out of memory/Memory allocation
  error`. A string token that fails UTF-8 validation now stores its own
  message, `Invalid UTF-8 in a string`. The accepted language is unchanged:
  both are refusals.
- **The scanner allocates from a pool** (see "Allocation failure" below).
- **Allocation-failure leaks and double frees in the grammar's actions** were
  fixed (see below).
- **The command** reports `name:line:column: message`, with exit statuses for
  usage and read errors, instead of ctang's "failed to compile".

Generated sources (`tangParser.c`, `tangScanner.c`, `flexTangScanner.h`) are
built into `build/<os>/<build>/generated/` and never committed.

## The interface

```c
GLTANG_Result gltang_parse(const char * source, GLTANG_ParseMode mode,
    GLTANG_ParseError * error_out, GLTANG_Tree ** tree_out);
```

ctang returned a tree whose root might be a parse-error node, or NULL, and NULL
meant both "there was nothing to parse" and "the parser could not allocate" -
which is how a syntax error once compiled into an empty program (ctang's
language reference, section 13.1). Here the result carries the distinction:

| Result | Meaning |
| --- | --- |
| `GLTANG_OK` | A tree; an empty source (whitespace and comments) is a tree with no root, count 0 |
| `GLTANG_ERR_FORMAT` | A syntax or scanner error; `error_out` says where (1-based line and column) and why |
| `GLTANG_ERR_LIMIT` | The source nests past bison's fixed stack (10,000 levels) |
| `GLTANG_ERR_OOM` | An allocation failed; nothing is left to free |
| `GLTANG_ERR_INVALID` | A NULL source or tree, or a bad mode |

An output parameter is written only on success, as CONVENTIONS section 5 says.
`error_out` is the one exception, deliberately: it is a diagnostic, written
when and only when the result is `ERR_FORMAT`, and optional.

`GLTANG_ERR_LIMIT` is bison's "memory exhausted", which it prints both when its
stack cannot grow and when the nesting reaches `YYMAXDEPTH`, and cannot tell
apart. The second is what a source can cause, so it is the one reported.

The tree is owned by its parse result and freed with it
(`gltang_tree_destroy`); the node classes are in the `ast/` headers, labelled
`free`, and the stable headers name only `GLTANG_Ast_Node`.

**Memory** is cutil's, exactly as ctang's was: every node and token buffer
comes from `gcu_malloc` and goes back with `gcu_free`, through
`gltang_allocator()`. Charging parse memory to a runtime context is **open for
the story that adds execution**: a context's counting allocator exists
(`runtime-core`), and the parse of a template is part of running it, but the
budget scope that would own the charge is that story's.

## Rejected alternatives

**Link ctang instead of porting its parser.** The spec forbids it and the
reasons are sound: a link makes lang-tang a client of a frozen library that is
meant to retire (AD-3: "ctang retires when the divergence ledger closes"), it
puts `GTA` names and ctang's allocator behaviour in every consumer, and it
makes the oracle measure a program against itself. The oracle is the one place
ctang is linked, and the runner there is a child process.

**Port the whole AST, with its compile, simplify and analyze slots.** The
tree would then keep every reference to `Program`, `Compiler_Context`,
`ComputedValue` and the bytecode, which are exactly what AD-9 says nothing is
built on; the library would have to carry or stub them. Stripping the tree to
`name`, `destroy`, `print` and `walk` leaves a front end that is complete on
its own, and the compiler of the next story adds its pass beside the nodes
(the headers are `free`, so that is allowed to change them).

**`GLTANG_` names versus reusing `GTA_`.** Two libraries in one process with
the same prefix is the situation CONVENTIONS section 4 exists to prevent, and
it is the situation the oracle test runner is in: it links ctang and lang-tang
together. Versioned symbols would keep the linker honest, but a reader would
still have two things called `GTA_Ast_Node`. AD-3 forbids `GTA` outright.

**flex and bison versus a hand-written parser.** The accepted language is
defined by ctang's grammar, and the point of the port is that it is *the same
grammar*: 1,800 lines of actions with a history of leak fixes that a rewrite
would have to rediscover one fuzz crash at a time. A hand parser would also
make the divergence ledger the place every ordinary parse difference landed.
The cost is a build dependency on two generators, and it is paid the way every
other generator in the suite is: a hard error naming what is missing, and the
output never committed.

**Keeping the AST's `print` writing to stdout versus taking a stream.** Ported
as is. `print` is a debugging dump (`_dump` in CONVENTIONS section 3), the
`tang` command and the tests are its only callers, and a stream parameter
would change forty signatures for a function whose output the oracle does not
compare. The tests capture stdout; the day a host wants the dump elsewhere,
that is one change in one header that is already labelled `free`.

**The ledger as a table versus per-test annotations.** A divergence is a fact
about the two engines, not about a test: the same departure shows up in a dozen
corpus files, and a per-test annotation would be a dozen places that can
disagree about whether it is recorded. A single table has one row per
departure, is read by a validator (unique ids, valid categories and statuses,
every named corpus file exists), and answers the question that retires ctang -
"is any row still open?" - with one `grep`. A table also makes a stale row
detectable: a `recorded` row that names a file that agrees fails.

**The oracle in-process versus a child process.** ctang crashes and hangs on
some inputs - that is part of why it is being replaced - and an in-process
oracle would take the test with it. A child with a wall-clock kill turns a
crash or a hang into a verdict. It also keeps ctang's `GTA_` symbols and its
global state out of the test process, which links lang-tang.

**Counting ctang's `killed` as agreement with lang-tang's `paused`.** The
spine's rule (AD-16), and the right one: a runaway template is stopped by the
harness on one side and paused on its budget on the other, and both are the
same observation. It agrees *only* with `paused`; a ctang that is killed where
lang-tang rejects is a divergence, and the unit tests show that. `paused` is
produced from the story that adds execution on, so nothing yet agrees with
`killed`.

**Comparing node counts versus comparing the printed tree.** The print text is
a debugging format that this library may change (the headers are `free`); the
node count is the same walk on both trees and cannot change without the tree
changing. It is also less than the whole truth - two different trees can have
equal counts - which is why the comparison is a first differential and not the
last one: execution output replaces it later and reuses the driver, the
comparison and the ledger rules.

## The oracle and the ledger

`make test-oracle` (part of `make test`) runs every corpus file through
`gltang_parse` and through ctang's own parse in a child (`tests/oracle/oracle_ctang`,
the one program here that includes ctang), compares the verdicts - `accept(n)`
with the node count, `reject`, ctang's `killed` - and holds the result against
`documentation/divergence-ledger.md`.

- It **fails when ctang is absent, and never skips.** `ORACLE_PC` names the
  package, and a bogus one is how the gate is seen to fail.
- A divergence is accepted only if a `recorded` row names that corpus file.
- A `recorded` row that names a file that agrees is **stale** and fails.
- An unrecorded divergence fails, naming the file.
- A runner that exits with a usage or read error, or prints something
  unreadable, is a failure of the harness and throws; only a signal or the
  wall clock is `killed`.

All of this is a pure function (`oracle::judge`) driven by planted cases in
`tests/unit/test_judge.cpp`, and the differential itself mutates a real
verdict and requires the judge to name the file.

At this commit the corpus has no divergence, so no row is `recorded` against a
file. The rows are the departures the spec lists: ctang's open section 13 items
(9 and 13), the per-context generator, budgets and the pause and unwind
outcomes, and error reporting. The ledger is closed when no row is `open`.

Two things the corpus found about ctang are worth recording here because they
are properties of the language and not of the port: `\r` is not whitespace, so
a CRLF source is a syntax error in both engines
(`script/reject-crlf-line-endings.tang`), and a digit sequence of four or more
digits after a backslash is a syntax error, not an octal escape followed by a
digit (`script/reject-octal-four-digits.tang`).

## Allocation failure

ctang's parser was never run with a failing allocator, and three kinds of
defect were found by doing so (`tests/unit/test_oom.cpp`, which fails each
allocation in turn):

1. **A node's `create` refused and the action leaked its operands.** The
   contract is that a refused creation leaves every argument with the caller,
   and about thirty rules did not honour their half: `a + b` with a failing
   `binary_create` dropped both operands. Each failing branch now discards what
   it was handed.
2. **A `create` that destroyed an argument it had adopted**, which the caller
   then freed again: `ranged_for_create` and `global_create`. They release only
   their own storage on failure. The `use IDENTIFIER;` rule freed its
   identifier and then destroyed the library node that had adopted it.
3. **Flex's allocations were fatal.** A failing `malloc` inside flex ends the
   process with `exit(2)`. The scanner's allocations - its state, the input
   copy, the buffer state and the start-condition stack - are served from one
   pool that `gltang_parse` reserves before the scan, sized by the input
   length plus a fixed slack, where failure is a return value. A pool request
   that cannot be served is a broken invariant and aborts with a message, not
   a silent `exit`.

Injection is a link-time `--wrap` of `malloc`, `calloc`, `realloc` and `free`
in the test binaries, so it works under ASan and TSan, where defining `malloc`
would not. It fails *one* allocation at a time: failing every allocation from a
point on would only show the first check working. Allocations that cutil makes
inside its own shared object are out of its reach, and ASan and Valgrind are
what watch those.

## The strict-aliasing exception

The suite's default is `-Wstrict-aliasing=1` (CONVENTIONS section 6, and the
`check-aliasing` gate that proves it is armed). The AST is built on the C
struct-inheritance downcast - `(GLTANG_Ast_Node_Binary *) self`, where `self`
points at the first member of that struct - which C17 6.7.2.1p15 makes well
defined and which level 1 reports 669 times in ctang. The ported AST and the
generated parser and scanner are therefore compiled at `-Wstrict-aliasing=3`
(`AST_CFLAGS` in the Makefile); everything else, including `src/parse/`, stays
at level 1. clang implements none of these levels and rejects `=3`, so under
clang the flag is empty.

## Packaging and edges

The library links `cutil` and `unicode` and nothing else. `runtime-core` and
`runtime-heap` are not `Requires` of the `.pc` - nothing here uses a symbol from
them yet - but the manifest lists them, as the spine says, so a bootstrap builds
them first.

`tools/check-edges.sh` enforces AD-2 over `src/`, `include/`, the generator
inputs, `bench/` and `examples/`, and over the NEEDED list of the shared library
and the `tang` command. The rule is an allowlist: `cutil`, `unicode`,
`runtime-core`, `runtime-heap`, `lang-tang`. Anything else - ctang, the
debugger, the JIT, another engine - is an edge, and so is any include of
`binary.h`. `tests/` is the one place ctang may be included, and a fixture shows
that an include there passes while the same line anywhere else does not.

`tools/check-labels.sh` classifies each header by name: the C embedding API
(`core.h`, `parse.h`, `libver.h`, `macros.h`, `namespace.h`, `allocator.h`, the
umbrella) is `stable`; everything under `ast/` and the headers the AST is built
on are `free`. A header in neither list fails, so a new header is a decision
about its stability taken on purpose.

## The `tang` command

A host (AD-2: apps in an engine's repository are hosts). It reads a file,
`-e SOURCE` or stdin, parses in template mode (`-s` for a script), and prints
the tree; a syntax error is `name:line:column: message` on stderr and exit 1;
usage errors exit 2, read failures 3, out of memory 4. `--help` says execution
arrives with the interpreter. `-c`/`--cleanup` is accepted for ctang
compatibility and does nothing: this command always releases what it
allocates. It is not installed, so it cannot shadow ctang's `tang`.

## Known defects, inherited and open

**Destroy, walk and print recurse with the depth of the tree.** A source such as
`1+1+1+...` is left-associative, so a chain of 10^5 terms is a tree 10^5 deep,
and `gltang_tree_destroy`, `gltang_tang_node_count` and `gltang_tree_print` each
recurse once per level. Measured here on the release build with an 8 MiB stack,
100,000 terms parse, count and destroy, and 400,000 overflow the C stack; the
limit moves with the compiler, the optimisation level and the sanitizer, since
it is a limit on frames. ctang has the same defect. `print` is also quadratic:
it builds an indent string one level longer at each depth. It is **recorded as
open for story 9**, which writes the compiler and the interpreter that walk the
tree and decides the depth budget; it is not fixed here, and the ledger does not
carry it because it is not a divergence from ctang.

The parser's own depth is bounded (`GLTANG_ERR_LIMIT` at 10,000 levels of
nesting); the tree's depth is not, which is the asymmetry.

## Gates

`make test` runs, in this order of concern: `check-symbols` (every exported
symbol carries the version token, every declared function the export macro,
every header includes `macros.h` and has a unique guard), `check-aliasing`,
`check-stamps` (every compile rule names a flag stamp that records what its
recipe expands), `check-labels`, `check-edges`, `check-gates` (every gate is
run against a planted defect, a control and an empty tree), the examples, the
unit tests, the CLI test, the fuzz replay, the oracle differential, and one
smoke run of the benchmark. `check-install` proves what `make install` leaves
behind is usable by a consumer that includes only the umbrella.

CI is not added: the spine defers it until each engine has a body, and `compress`
and `text` are the libraries that have it. A local `make test` is the whole gate.

## Benchmarks

`bench/bench.c` is the harness (AD-26), here from the first commit: a
calibration case beside parse of a small script, parse of a 1 MiB generated
template, and destruction of that template's tree. `make test` runs it once
with a tiny workload; `make bench` runs it in full. No numeric budget is
asserted. The first measurement, on the development machine (gcc -O2, one run),
for reading future figures against the calibration beside them:

| Case | Figure |
| --- | --- |
| calibration | 1.1 ns per xorshift step |
| parse of the small script | about 4.2 us |
| parse of a 1 MiB template (about 11,000 rows) | about 44 ms |
| destroy of that tree | about 18 ms |

## Fuzzing

`tests/fuzz/fuzz_parse.c` and `fuzz_template.c` are libFuzzer harnesses on
`gltang_parse` (`make fuzz-parse`, `make fuzz-template`, run with
`make fuzz-run-parse`; clang). `make fuzz-replay` feeds every corpus and seed
file once through the same entry points in an ordinary gcc build and fails on a
crash - it is part of `make test`, so a regression a fuzzer once found is a
failing test and not a campaign to repeat. No campaign is run here, and no
differential fuzzing: that is the story that compares execution.

## What is not here

No bytecode, compiler, interpreter, `Program` or execution context, no
`ComputedValue`, no library registry or `random`, no host API, no budgets and
no debugger (later stories). No `simplify`. No CI. The ledger's open rows
(sections 13.9 and 13.13 of the language reference) are decisions the compiler
and interpreter take.
