# Design

**Status:** In progress. Describes what exists: the front end of the Tang
engine (the parser, the scanner and the syntax tree, ported from ctang as this
library's own source), the compiler from that tree to this library's own
bytecode, the switch-dispatched interpreter that runs it on a runtime-core
context and a runtime-heap heap, the interface that parses, compiles and runs a
template or a script, the `tang` command over it, the divergence ledger, and
the oracle that compares this library with frozen ctang. What is still not
here is listed at the end. The architecture it follows is the runtime stack's
spine (AD-2, AD-3, AD-9, AD-13, AD-14, AD-16, AD-21, AD-26).

## What this library is

`lang-tang` is the Tang engine on the new runtime stack, and it replaces ctang.
The language is ctang's, unchanged: a template language whose code tags and
scripts share one grammar, with errors as values. What changes is everything
under the language - a new bytecode, a new interpreter, budgets, a collector,
a debugger. The first piece was **the tree**: the parser, the scanner and the
AST are ported from ctang, and the oracle and the ledger that keep the engine
honest exist before any behaviour that could drift. The second is **the
engine**, described under "The engine" below: a compiler, an interpreter, the
value model and the execution API. Libraries (`math`, `random`, the host's
own), native functions and the error list are the next story's.

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

## The engine

The path from source to answer is four calls, each a library of its own
concern: `gltang_parse` (source to tree), `gltang_compile` (tree to program),
`gltang_execution_create` (a program on a runtime-core context that already has
a runtime-heap heap) and `grcore_run(context, gltang_execution_entry,
execution, &outcome)`. The host owns the context, so the host owns the budgets
(fuel, memory, guest depth), the thread it runs on, and what to do when a run
pauses. A program is immutable and reference counted, so many contexts on many
threads may run the same one; an execution belongs to one context and is the
context's keyed state (cardinality one), destroyed with it.

### Values

A value is one 64-bit word with a tag in the low four bits.

| Tag | Meaning |
| --- | --- |
| `0` | A heap pointer, 16-byte aligned. The word `0` is the null pointer and also the language's null. |
| `1` | A small integer, 60 bits signed. An integer outside that range is boxed. |
| `2` | A boolean: `0x02` false, `0x12` true. |
| `3` | A function value; the function index is in the upper bits. |
| `0xF` | `V_UNWIND`, the interpreter's own sentinel for "a run is unwinding"; never stored. |

Floats and large integers are boxed heap objects. The heap is told this through
`gltang_heap_options_configure`, which sets the codec (tag mask `0xF`, tag
value `0`, mask `UINT64_MAX`), so the collector treats a word with a non-zero
tag as a number and nothing else. The first `u32` of every heap payload is its
kind: integer, float, string, array, array store, map, map store, error.

An array and a map are a small header plus a **replaceable storage object**:
growing allocates a new store and swaps it in, so the header's address, which
other values hold, never changes. The `store` field is a union of `void *` and
the typed pointer so the collector's write barrier (`grheap_store`) is the only
way it is written. Maps keep insertion order, with an open-addressing `u32`
index.

A string is one flat heap block: kind, segment count, byte length, grapheme
length, a segment array of `(type << 32) | first_grapheme`, a `u32` offsets
table only when the grapheme length differs from the byte length (so ASCII
costs nothing), and the bytes. Concatenation and substring do not re-break
graphemes (ctang's behaviour); rendering re-breaks through the unicode
library's `from_utf8`. The segment tag is what lets the output carry typed
pieces: `print` appends a value's segments, and `gltang_execution_output_render`
encodes each by its tag (text, HTML, HTML attribute, JavaScript, URL, trusted),
`gltang_execution_output_raw` does not.

An **error is a value**, as in ctang, and a heap object: a kind (13 of them,
including the three `[...]` markers that print as themselves), and the origin
it was created at - file, line, function index, bytecode offset - which the
host reads with `gltang_execution_result_error`. An error prints as nothing
(a marker prints as itself) and inside a container prints as `Error: message`.
The origin is the same pair a poll uses, so the error list of the next story
and a debugger name the place in the same terms.

**Rejected: NaN boxing.** It would make floats free and put every pointer in 48
bits, but the collector's codec describes a tag in the low bits and a mask, not
a NaN pattern, and the 16-byte alignment the heap hands out leaves the four bits
for free. **Rejected: tagged strings inline.** Short strings in the word would
save an allocation for small keys, but a string's segments and graphemes make a
short one no simpler, and the second representation would double the code in
every operation that takes a string.

### Bytecode

A stack machine, not a register machine. The compiler is a single pass over the
tree and a stack machine's code is what that pass produces without allocating
registers; the interpreter's frame size is then `4 + locals + max_stack`, and
`max_stack` is computed by an abstract-interpretation pass over each function
(so a program that would overflow its frame is a compile defect, not a runtime
one). Registers would make the code shorter and the compiler longer, and the
measured cost of a stack op here (the benchmarks below) is a few nanoseconds.

An instruction is a 32-bit word: the opcode in the low 8 bits, an operand in
the high 24, so a function holds up to 16,777,215 instructions, constants and
locals, and a program over that is refused with `GLTANG_ERR_LIMIT`. The
dispatch is a `switch`; computed goto would be faster and is not portable to
the compilers the suite builds with, and AD-21's budget is counted in
instructions either way.

| Opcode | Effect |
| --- | --- |
| `POLL` | Poll: function entry and every loop back-edge. |
| `POP` | `v --`. |
| `DUP` | `v -- v v`. |
| `NULL` | `-- null`. |
| `TRUE` | `-- true`. |
| `FALSE` | `-- false`. |
| `CONST` | `-- k`: constant `a` of the program's pool. |
| `LOAD_LOCAL` | `-- v`: frame local `a`. |
| `STORE_LOCAL` | `v -- v`: frame local `a`. |
| `LOAD_GLOBAL` | `-- v`: program-scope variable `a`. |
| `STORE_GLOBAL` | `v -- v`: program-scope variable `a`. |
| `FUNC` | `-- f`: the function value of function `a`. |
| `SET_RESULT` | `v --`: the program's result becomes `v`. |
| `CLEAR_RESULT` | The program's result becomes null. |
| `USE` | `-- v`: resolve the library path of constant `a`. |
| `NEG` | `v -- -v`. |
| `NOT` | `v -- !v`. |
| `ADD` | `a b -- a+b`. |
| `SUB` | `a b -- a-b`. |
| `MUL` | `a b -- a*b`. |
| `DIV` | `a b -- a/b`. |
| `MOD` | `a b -- a%b`. |
| `LT` | `a b -- a<b`. |
| `LE` | `a b -- a<=b`. |
| `GT` | `a b -- a>b`. |
| `GE` | `a b -- a>=b`. |
| `EQ` | `a b -- a==b`. |
| `NE` | `a b -- a!=b`. |
| `JMP` | Jump to instruction `a`. |
| `JMP_FALSE` | `v --`: jump to `a` if `v` is falsy. |
| `JMP_TRUE` | `v --`: jump to `a` if `v` is truthy. |
| `AND` | `v -- v` and jump to `a` if falsy; `v --` otherwise. |
| `OR` | `v -- v` and jump to `a` if truthy; `v --` otherwise. |
| `CAST` | `v -- c`: to int, float, bool, string (`a` is GLTANG_Cast_Type). |
| `INDEX` | `c i -- v`. |
| `ATTR` | `c -- v`: attribute named by constant `a`. |
| `SLICE` | `c [s] [e] [t] -- v`: `a` bit 0 start, 1 end, 2 step. |
| `SET_INDEX` | `c i v -- v`; `a` is 1 when `v` is copied first if it is a container. |
| `SET_ATTR` | `c v -- v`: member named by constant `a >> 1`; bit 0 as for SET_INDEX. |
| `ADOPT` | `v -- v'`: a deep copy of a container, else `v`. |
| `ARRAY` | `e1..en -- array`, `a` = n. |
| `MAP` | `k1 v1..kn vn -- map`, `a` = n. |
| `CALL` | `f a1..an -- v`, `a` = n. |
| `RET` | `v --`: return `v` to the caller. |
| `PRINT` | `v -- null`: append `v` to the output. |
| `PRINT_CONST` | Append string constant `a` to the output. |
| `ITER_INIT` | `v -- bool`: start iterating array `v` over locals `a`, `a+1`. |
| `ITER_NEXT` | `-- e` or jump: two words, the second is the exhausted target. |

`ITER_NEXT` takes two words: the second is the exhausted target. Every
instruction costs one unit of fuel; the operations that do work proportional to
a size (copy, concatenate, compare, print, render) add one unit per
`GLTANG_WORK_BYTES_PER_FUEL` (64) bytes, and an element of a container counts
as 8 bytes.

### Frames, polls, fuel

A Tang-to-Tang call pushes a frame on the context's **guest stack**
(runtime-core's), not on the C stack, so a recursion of 50,000 frames runs on an
ordinary 8 MiB C stack and the guest-depth budget (not the C stack) bounds it.
A frame is slots: four raw header words (function index, pc, sp, flags), then
the locals, then the operand stack, all of which the collector scans as values
except the header, which the frame protocol marks `GRCORE_SLOT_RAW`. Operand
slots are zeroed when popped so a dead value is not kept alive. The interpreter
reloads its slots pointer after every operation that can allocate or grow the
stack, because both can move it; the harness's always-move stack exists to prove
that.

**The call-depth rule.** The guest-depth budget is ctang's `max_call_depth`
plus one: the top-level frame counts, ctang's does not. With a limit of 10, a
function can nest 10 calls; the eleventh is the error value `Recursion Limit
Exceeded`, and the program goes on.

A **poll** is the first instruction of every function and the head of every
loop, so `continue` polls too. The poll identity is `(function index,
instruction index)`; it is stable across runs and a pause reports it, with the
file and line, through `grcore_context_pause_location`. Fuel is charged in a
batch held in the execution and flushed before every poll and on every exit, so
the figure a host reads is exact at the places it can read it.

A run that cannot continue unwinds: the engine descriptor's unwind hook counts
the frames it pops, `gltang_execution_unwound_frames` reports them, and the run
ends with `GRCORE_ERR_LIMIT` (and the state `UNWOUND`). Pause and unwind are the
host's decision, not the program's.

**Natives and the runtime poll.** An operation whose work is proportional to a
size cannot reach a poll instruction while it works, and AD-21 forbids a native
from pausing, so it calls `grcore_runtime_poll` every 4,096 bytes of work
(`GLTANG_POLL_BYTES`). That poll can only vote to unwind. A half-built object is
temp-rooted (the execution's `temps`) while the poll runs, so a collection the
poll triggers does not free what the operation is building. A runaway
`[0] * 1000000000` under a small fuel budget therefore ends in a bounded time
with the limit error, not at the memory ceiling (tested in
`tests/unit/test_engine.cpp`).

**Memory.** An allocation that returns `GRHEAP_ERR_LIMIT` triggers
`grcore_runtime_poll`, which runs a collection and counts the memory verdict;
if that votes to unwind, the run unwinds, and otherwise the operation yields the
error value `Out of memory`, which is preallocated per execution (so producing
it cannot itself fail). The execution is a root source reporting `roots[]`
(result, that error, the program-scope variables, a cache of materialised
constants) and `temps[]`.

### Names

Resolution is static. A program-scope variable is a global slot, a function's
variable is a frame slot, and a ranged `for` uses two hidden locals. A name
used before a function of that name is declared is a "declared twice" compile
error, as in ctang. Inside a function, a program-scope function declared
earlier is visible by name, and assigning to it is refused. A top-level
`global`, a repeated parameter name and an assignment target that is not
assignable (`Cannot assign to this expression.`) are compile errors, reported
as `file:line:column: message`.

`use` is the **resolver seam**: `USE` calls the execution's resolver with the
whole dotted path and binds whatever it returns (an integer, float, string,
bool or null, as plain data) to the name; no resolver, or a path it does not
know, binds null. Story 10 replaces the resolver's body with the library
registry and native functions; nothing else in the engine changes.

### The tree-depth budget

Story 8 left destroy, walk, count and print recursing once per level of the
tree, and print quadratic. The parser now refuses a tree deeper than
`GLTANG_MAX_TREE_DEPTH` (10,000, the same figure as bison's nesting limit)
with `GLTANG_ERR_LIMIT` and no tree: each node records its height as it is
created and the create function refuses past the limit, so the bound holds for
every tree that exists and destroy, walk, count, print and the compiler are
bounded by it. Measured: `1+1+...` of 9,999 pluses (10,000 deep) parses,
compiles and runs, and 10,000 pluses is refused, under the plain build and
under ASan, whose frames are larger. `gltang_tree_print` caps its indent at 256
levels, which makes it linear. Ledger rows D-016 and D-017.

### The memory-budget contract

The execution's allocations are the context's: the heap charges them to the
context's counting allocator, so a memory budget set on the context is a budget
on the program. The parse is not charged (the parser's allocations are still
cutil's, uncounted). That is the one thing story 8 deferred to this story that
this story leaves open, and the reason: the parse happens before the context
exists in the `tang` command and in every example, and charging it needs the
parser to take an allocator (a change to the stable `gltang_parse` signature).
It is carried forward, not dropped.

### The ported tests

`tests/unit/test_execute_simple.cpp` and `test_execute_complex.cpp` are ctang's
two execution suites ported to this API, through `tests/exec_harness.h`, which
supplies the counting allocator and page provider that prove nothing leaks, the
`Context` that runs a program and reads its result, and the environment switches
`GRHEAP_TORTURE`, `GRHEAP_VERIFY` and `GLTANG_TEST_MOVING_STACK`. They run
plain, with the heap collecting before every allocation and verifying every
store, and with a stack that moves on every push; `make test-torture` runs them
under ASan and UBSan as well.

Every ctang test has a row here. 88 of the 89 in the simple suite and all 28 in
the complex suite are ported, with the same inputs and expected values
wherever the departure ledger does not say otherwise.

| ctang test | Disposition |
| --- | --- |
| `Binary.CodeBlockIsUnmappedOnDestroy` (simple) | **Dropped.** It tests ctang's JIT code block, which this library does not have (story 8's rule: nothing is built on ctang's bytecode or `binary.h`). |
| `NativeFunction.Library` (complex) | **Partly ported.** The "function not found" case is ported. The three cases that call a native function value supplied by the host's library (no arguments, two arguments, bound to an object) need the library registry and native functions, which are **story 10's**. |
| `Function.TheCallDepthLimitIsTheHostsToSet` and `Function.ALocalHasASlotOfItsOwn` (complex) | Ported with the call-depth rule above (budget = `max_call_depth` + 1); the second uses `return` because falling off the end gives null (D-010). |
| `Function.RecursionIsBounded` (complex) | Ported; the 100,000-deep case runs at 3,000 in the moving-stack variant, where each push copies the stack. |
| every other test in both files | Ported. |

Added beside them: `Function.FallingOffTheEndReturnsNull` (D-010).
The probes that decided the behaviour of every row (ctang against lang-tang over
about 500 snippets, and over the 35 template and 108 script corpus files) found
only the departures in the ledger: D-009 to D-015, D-017 and D-019.

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
umbrella) is `stable`; everything under `ast/`, the headers the AST is built on,
and the engine's (`bytecode.h`, `program.h`, `compile.h`, `value.h`,
`execution.h`) are `free`: they are new, and a header joins the stable set by a
decision to freeze it. A header in neither list fails, so a new header is a decision
about its stability taken on purpose.

## The `tang` command

A host (AD-2: apps in an engine's repository are hosts). It reads a file,
`-e SOURCE` or stdin, parses, compiles and **runs** it, and writes the rendered
output to stdout: a template by default for a file or stdin, `-s` for a script,
and `-e` is a script (`-t` makes it a template; D-019). `--tree` prints the
tree instead of running. `--fuel N` and `--depth N` set the budgets. A syntax or
compile error is `name:line:column: message` on stderr and exit 1; usage
errors exit 2, read failures 3, out of memory 4, a run paused for fuel 5
(`name:line: paused on <keys>`), and a run unwound 6. `-c`/`--cleanup` is
accepted for ctang compatibility and does nothing: this command always releases
what it allocates. It is not installed, so it cannot shadow ctang's `tang`.

## Known defects, inherited and open

**The parse is not charged to the context.** See "The memory-budget contract".

ctang's bytecode engine hangs on a program that ends in a function
declaration, and crashes on `function f(f)`. Neither is inherited: lang-tang
does not share the engine (D-011).

**A float literal is read with `strtold`**, as in ctang's scanner, and narrowed
to a double. On x86-64 that is the 80-bit type and the narrowing rounds once;
under Valgrind, which emulates `long double` as a double, `9223372036854775807.0`
reads as 2^63 - 1024. The one test whose answer depends on that boundary
(`Cast.OutOfRangeFloatToIntegerSaysSo`) skips that block when the host's
`strtold` does not give 2^63, and `make test-valgrind` passes. Reading the
literal with `strtod` would remove the dependency and is a change to the
scanner's rule, left for the story that touches it.

Destroy, walk, count and print used to recurse with the depth of the tree
(story 8's known defect). That is closed by the tree-depth budget above.

## Gates

`make test` runs, in this order of concern: `check-symbols` (every exported
symbol carries the version token, every declared function the export macro,
every header includes `macros.h` and has a unique guard), `check-aliasing`,
`check-stamps` (every compile rule names a flag stamp that records what its
recipe expands), `check-labels`, `check-edges`, `check-gates` (every gate is
run against a planted defect, a control and an empty tree), the examples, the
unit tests, the same engine suites again with the heap in torture and verify
mode and again with a stack that moves on every push, the CLI test, the fuzz
replay (three harnesses), the oracle differential, and one smoke run of the
benchmark. `make test-torture` repeats the engine suites under ASan and UBSan
and `make test-tsan` runs every suite under ThreadSanitizer, including the ones
that hop a paused context from thread to thread. `check-install` proves what `make install` leaves
behind is usable by a consumer that includes only the umbrella.

CI is not added: the spine defers it until each engine has a body, and `compress`
and `text` are the libraries that have it. A local `make test` is the whole gate.

## Benchmarks

`bench/bench.c` is the harness (AD-26): a calibration case beside the front
end (parse of a small script, parse of a 1 MiB generated template, destruction
of its tree, compile of both) and the engine (a tight integer loop, a recursive
`fib(15)`, building a string, building an array, and the loop again paused and
resumed every 500 units of fuel). An engine case's unit is one run of a fixed
program, timed from `grcore_run` to its return. `make test` runs it once with a
tiny workload; `make bench` runs it in full. No numeric budget is asserted. The
first measurement, on the development machine (gcc -O2, best of seven), for
reading future figures against the calibration beside them:

| Case | Figure |
| --- | --- |
| calibration | 1.4 ns per xorshift step |
| parse of the small script | about 4.5 us |
| parse of a 1 MiB template (about 11,000 rows) | about 46 ms |
| destroy of that tree | about 19 ms |
| compile of the small script | about 1.4 us |
| compile of the 1 MiB template | about 7 ms |
| run: 1,000-iteration integer loop | about 65 us (65 ns per iteration, about a dozen instructions) |
| run: `fib(15)` (1,973 calls) | about 181 us (about 92 ns per call) |
| run: 200 string appends | about 40 us |
| run: 1,000 array element stores | about 82 us |
| run: the loop paused and resumed every 500 fuel | about 69 us (the pauses add about 6%) |

## Fuzzing

`tests/fuzz/fuzz_parse.c` and `fuzz_template.c` are libFuzzer harnesses on
`gltang_parse` and, for an accepted tree, `gltang_compile`; `fuzz_run.c` parses,
compiles and runs, under a small fuel, memory and depth budget, so a loop or a
runaway recursion ends as a pause or an unwind (neither is a failure) and the
result and output are read after (`make fuzz-parse`, `make fuzz-template`,
`make fuzz-run`; run with `make fuzz-run-parse` and so on; clang). The first
byte of a `fuzz_run` input picks script or template. `make fuzz-replay` feeds
every corpus and seed file once through the same entry points in an ordinary gcc
build and fails on a crash - it is part of `make test`, so a regression a fuzzer
once found is a failing test and not a campaign to repeat. Fifteen seconds of
`fuzz_run` (307,000 executions, 2,940 new units) found nothing on the first
run. No campaign is run here, and no differential fuzzing: that is the story
that compares execution against ctang over generated programs.

## What is not here

No libraries (`math`, `random`, `string`, the host's own), no native function
values, no library registry (the resolver seam is where it goes), no error list
or halt-on-first-error option, no logging of errors at creation (all story 10);
the execution differential against ctang and the closing of the ledger (story
11). No debugger beyond the frame protocol the engine registers (the frame
walk, scopes and variables read from a paused context). No `simplify`. No
parse-time charge to a context's memory (see "The memory-budget contract"). No
CI. The ledger's open row (section 13.9 of the language reference, D-001) is a
decision the error list takes.
