# Design

**Status:** In progress. Describes what exists: the front end of the Tang
engine (the parser, the scanner and the syntax tree, ported from ctang as this
library's own source), the compiler from that tree to this library's own
bytecode, the switch-dispatched interpreter that runs it on a runtime-core
context and a runtime-heap heap, the interface that parses, compiles and runs a
template or a script, the host API over it (libraries, native functions, the
error list, template calls under budget scopes, a generator per context), the
`tang` command, the divergence ledger, the oracle that compares this
library with frozen ctang, and a baseline JIT behind a build option
(`JIT=yes|no`, "The baseline JIT") that is compared with the interpreter at every
poll, and context snapshots ("Snapshots": a paused or new execution frozen
and restored, into another context on any thread, to finish as an uninterrupted
run does), and the two services a developer asks of a running program: a
sampling profiler and a retention query ("Profiling and retention"). What is
still not here is listed at the end. The
architecture it follows is the runtime stack's spine (AD-2, AD-3, AD-9, AD-13,
AD-14, AD-16, AD-20, AD-21, AD-22, AD-23, AD-25, AD-26).

## What this library is

`lang-tang` is the Tang engine on the new runtime stack, and it replaces ctang.
The language is ctang's, unchanged: a template language whose code tags and
scripts share one grammar, with errors as values. What changes is everything
under the language - a new bytecode, a new interpreter, budgets, a collector,
a debugger. The first piece was **the tree**: the parser, the scanner and the
AST are ported from ctang, and the oracle and the ledger that keep the engine
honest exist before any behaviour that could drift. The second is **the
engine**, described under "The engine" below: a compiler, an interpreter, the
value model and the execution API. The third is **the host API**, under "The
host API": libraries (`math`, `random`, the host's own), native functions, the
error list, and template calls that each open a budget scope.

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
kind: integer, float, string, array, array store, map, map store, error, library,
native function, template, generator.

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

An **error is a value**, as in ctang, and a heap object: a kind (16 of them,
including the three `[...]` markers that print as themselves), and the origin
it was created at - file, line, function index, bytecode offset - which the
host reads with `gltang_execution_result_error`. An error prints as nothing
(a marker prints as itself) and inside a container prints as `Error: message`.
The origin is the same pair a poll uses, with the index of the program it is in,
so the error list and a debugger name the place in the same terms.

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
| `SET_RESULT` | `v --`: the program's result becomes `v`; operand 1 marks an expression statement that can lose an error, which the error list enters if a later statement replaces it. |
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
| `DISCARD` | `v --`: the value of an expression statement that can lose an error; an error is entered in the error list. |
| `LINE` | The start of a statement. A poll if the execution asked for statement polls, otherwise nothing. Costs no fuel. |

`ITER_NEXT` takes two words: the second is the exhausted target. Every
instruction costs one unit of fuel, except `LINE`, which costs none (see
"Statement polls" below); the operations that do work proportional to
a size (copy, concatenate, compare, print, render) add one unit per
`GLTANG_WORK_BYTES_PER_FUEL` (64) bytes, and an element of a container counts
as 8 bytes.

### Frames, polls, fuel

A Tang-to-Tang call pushes a frame on the context's **guest stack**
(runtime-core's), not on the C stack, so a recursion of 50,000 frames runs on an
ordinary 8 MiB C stack and the guest-depth budget (not the C stack) bounds it.
A frame is slots: four raw header words (function, pc, sp, flags), then
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
loop, so `continue` polls too. The poll identity is `(function word,
instruction index)`, where the function word is the function index with the
index of its program in the high 32 bits (0 for the main program); it is stable across runs and a pause reports it, with the
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

### Statement polls

AD-4 puts a poll at function entry, at every loop back-edge and at throw points,
and that is all a budget needs. A debugger needs more: a breakpoint can only
fire where the engine polls, so with those sites alone a breakpoint on a line in
a straight run of statements never fires, and a step over a statement has
nowhere to stop (runtime-debug's design.md states that as the contract an engine
has to meet). **This was an extension of AD-4's list of poll sites, and the spine now
ratifies it: AD-4 lists the statement boundary among its poll sites (commit
`afc292d` of the workspace).** It is opt-in, so that nothing in the suite moves
unless a host asks.

The `LINE` instruction is emitted at the start of every statement
`compile_statement` handles, except a block (a block is the list of the
statements inside it, each of which has its own, so a brace on a line of its
own is not a place to stop), and at the top level of a template or a script: the
text of a template is a statement per piece of text, a `<% %>` tag is its
statements. It is mapped into the line table like any instruction, so it has the
statement's first line. It costs **zero fuel** (`gltang_opcode_cost_table`), so
the fuel a program is charged is the same with the option on or off, and a
budget means the same thing to a host that turns it on. Its body is

    case GLTANG_OP_LINE:
      if (exec->statement_polls) { /* exactly the POLL body */ }
      break;

so with `gltang_execution_set_statement_polls(execution, false)` (the default)
it is one load and one branch, and polls, fuel, pause locations and frame traces
are what they were before the opcode existed (the suite shows it: no existing
expected value changed except the one that is a property of the bytecode, the
cost table test's "every opcode costs at least one", which now names `LINE` as the
one that costs none). With
it on it is the `POLL` body, so the debugger, the observer and the fuel check see
nothing new but a more frequent engine. The engine never asks whether a debugger
is attached (AD-2): the host that attaches one turns the option on, on that
execution, and the setting is per execution because a program is shared by
every context (refcounted bytecode, AD-14).

**Granularity, stated and not hidden.** A line breakpoint fires at the first
`LINE` poll of a statement that starts on that line. A line on which no
statement starts (a blank line, a comment, the second line of a statement that
began on the line before, a closing brace) never fires; the DAP adapter still
reports it `verified`, which is the limitation runtime-debug records. A
multi-line statement fires at its first line. A line shared by an entry poll or a
loop's back-edge poll and the statement that follows it is more than one poll
and a breakpoint stops at each: a loop on a line of its own stops at the
statement `LINE` and then at its back-edge `POLL`, which is the same line and
one `continue` away; stepping is not affected, because a step stops only at a
poll whose (location, depth) differs from where it started.

**Cost.** Measured with the benchmark harness (gcc -O2, best of five a case, two
rounds alternating the tree before the opcode, commit 83c1fac, with this one, the
calibration case beside them, round-to-round noise about 1%). Statement polls
off: the instruction is executed and does nothing (the dispatch, and the table
lookup that adds its zero to the pending fuel), which costs about 3% on the
1,000-iteration loop (64.6 to 66.7 us), 6% on `fib(15)` (181 to 192 us), 3% on the
template-call case (107.5 to 111.1 us) and 3% on the polling loop (67.6 to 69.8
us). On with nothing pending, the poll's unarmed fast path (flush the fuel,
save the frame, `grcore_stack_poll`) costs about 16 ns a statement: the
four-statement loop body of `run-statements-1000-polls-*` goes from about 65 us
to about 130 us, about 4,000 statements. Both numbers are in the table under
"Benchmarks". No budget is asserted (AD-26).

**Rejected alternatives.**

- *Poll at every instruction.* It moves every pause location, every fuel flush
  and every observer trace in the suite, and costs an order of magnitude more.
- *Patch the bytecode per context when a debugger attaches.* A program is
  shared by every context and refcounted; per-context patching would mean a copy
  of the code per context, or a patch that other contexts see.
- *A debugger flag on the program.* It would make a property of the host's
  context a property of a shared, immutable program.
- *A breakpoint opcode swapped in at the line.* The classic debugger design
  needs writable code and a map from lines to instruction offsets that the
  engine would have to keep; it is the same patching, and the same sharing
  problem, with a harder undo.
- *Poll in `compile` only on lines that have a breakpoint.* The compiler would
  have to know the host's breakpoints, which are set after compilation, are
  per context, and change while a program runs.

### Names

Resolution is static. A program-scope variable is a global slot, a function's
variable is a frame slot, and a ranged `for` uses two hidden locals. A name
used before a function of that name is declared is a "declared twice" compile
error, as in ctang. Inside a function, a program-scope function declared
earlier is visible by name, and assigning to it is refused. A top-level
`global`, a repeated parameter name and an assignment target that is not
assignable (`Cannot assign to this expression.`) are compile errors, reported
as `file:line:column: message`.

`use` resolves through the library registry, described under "The host API":
the first name of the path is looked up in the execution's library, then the
program's, then the built-ins, and the rest of the path is read with the
engine's attribute rule. A name nothing provides binds null.

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
cutil's, uncounted). That is the one thing story 8 deferred that stories 9 and 10
leave open, and the reason: the parse happens before the context exists in the
`tang` command and in every example, and in the host API it also happens at
template registration, before the library that holds the program is attached to
any context; charging it needs the parser to take an allocator (a change to the
stable `gltang_parse` signature). It is carried forward, not dropped. Everything
the host API allocates *inside* a run - the error list, the activation records,
the program table, a native's answer - is charged to the context.

### The ported tests

`tests/unit/test_execute_simple.cpp` and `test_execute_complex.cpp` are ctang's
two execution suites ported to this API, through `tests/exec_harness.h`, which
supplies the counting allocator and page provider that prove nothing leaks, the
`Context` that runs a program and reads its result, and the environment switches
`GRHEAP_TORTURE`, `GRHEAP_VERIFY` and `GLTANG_TEST_MOVING_STACK`. They run
plain, with the heap collecting before every allocation and verifying every
store, and with a stack that moves on every push; `make test-torture` runs them
under ASan and UBSan as well.

Every ctang test has a row here. 88 of the 89 in the simple suite, all 28 in the
complex suite and all 4 in the library suite are ported, with the same inputs
and expected values wherever the departure ledger does not say otherwise.

| ctang test | Disposition |
| --- | --- |
| `Binary.CodeBlockIsUnmappedOnDestroy` (simple) | **Dropped.** It tests ctang's JIT code block, which this library does not have (story 8's rule: nothing is built on ctang's bytecode or `binary.h`). |
| `NativeFunction.Library` (complex) | **Ported, all four cases** (story 10): no arguments, two arguments, bound to an object (the host function's `user` pointer is the bound state), and "function not found" (`start  end`). They are in `test_execute_complex.cpp` under the original name. |
| `Library.Load`, `Math.Constants`, `Library.UseAs`, `Random.Random` (`test-tangLanguageLibrary.cpp`, 4 tests) | **Ported** (story 10) as `Library.Load`, `Math.Constants` and `Library.UseAs` in `test_library.cpp` and `Random.Random` in `test_random.cpp`, with the same inputs and expectations, read through the result's kind and the output. `Random.Random`'s checks of ctang internals (the global is the process's singleton, `default` has a seed, the seed of `seeded(123)` is 123) are the kinds and the bit-exact words from `std::mt19937_64`, since the global is now per execution (D-003) and a generator has no public seed. |
| `Function.TheCallDepthLimitIsTheHostsToSet` and `Function.ALocalHasASlotOfItsOwn` (complex) | Ported with the call-depth rule above (budget = `max_call_depth` + 1); the second uses `return` because falling off the end gives null (D-010). |
| `Function.RecursionIsBounded` (complex) | Ported; the 100,000-deep case runs at 3,000 in the moving-stack variant, where each push copies the stack. |
| every other test in both files | Ported. |

Added beside them: `Function.FallingOffTheEndReturnsNull` (D-010).
The probes that decided the behaviour of every row (ctang against lang-tang over
about 500 snippets, and over the 35 template and 108 script corpus files) found
only the departures in the ledger: D-009 to D-015, D-017 and D-019. The host API
was probed the same way, with about 60 snippets against a built ctang (every
operation on a library, a generator and a native function, the argument checks
of `seeded` and `set_seed`, `use` of unknown and dotted names) and the three new
corpus files `use-math.tang`, `use-random-seeded.tang` and
`use-unknown-library.tang`, which print the same bytes on both engines; the
departures they found are D-012, D-021 and the rest of D-003.

## The host API

A program is given data and functions by its host, learns of the errors it
swallowed, and calls other templates, through four things that the language does
not see: a library registry, an error list, template calls under budget scopes,
and a generator per context. The language gains no syntax and no semantics from
any of them (the spec): `use` binds a value, calling a value is a call, and an
error is still a value. Two readings of the architecture are made here, because
it did not settle them.

1. **The seed sequence is lang-tang's.** AD-25 and CAP-6 say a generator's seed
   comes from "the group's seed sequence", and runtime-core's group has no policy
   slot for one. The sequence is `GLTANG_SeedSequence` (`seeds.h`), which the
   host makes once per group and hands to each execution. Moving it into
   runtime-core is a later story's, and only the place the host puts it changes.
2. **AD-21's three scope outcomes are the host policy of a template
   registration.** "Nothing, completed segments, pause" are
   `GLTANG_SCOPE_EMPTY`, `GLTANG_SCOPE_SEGMENTS` and `GLTANG_SCOPE_PAUSE`, chosen
   when the host registers the template, because the host is who decides what a
   cut-off pane is worth and the program must not.

### Libraries

A `GLTANG_Library` is **a named table of members**: null, boolean, integer,
float and string values (a string carries its encoding tag), native functions,
templates, other libraries, and lazy factories. It is built with the
`gltang_library_add_*` calls, reference counted with an atomic count, and
**sealed** the moment it is attached anywhere - to an execution, to a program, or
as another library's member. A sealed library refuses every mutation with
`GLTANG_ERR_INVALID`.

Sealing is the whole concurrency story. A registry is shared by many contexts on
many threads and read with no lock, because nothing can write it; a program stays
immutable, which AD-22 asks for, because the libraries it was given cannot change
under it; and a cycle of libraries cannot be built, because to put A inside B the
host has first sealed A, so A can never be given B. The registry root is an
unnamed library; a named one is a member of its parent under its own name, and the
name is what `Library: math` prints. Library memory is cutil's, not a context's:
a host builds it before any context exists and it outlives them all.

`math` and `random` are the **third layer**, written in C in `src/library/builtin.c`
as static, sealed tables: `math` has `pi` (a float) and every other name is
`Not implemented`; `random` has `global`, `default` and `seeded`, which the engine
implements because they need an execution.

**Rejected: one registry per execution that the guest could extend** (what ctang
has: `use` can reach a library the program itself loaded). It would give each
execution a mutable table, which is the lock the sealing rule removes, and nothing
in the reference needs a program to add to its own libraries.
**Rejected: a callback that resolves every `use`** (story 9's seam). It left the
re-entrancy and thread rule unstated and made every name a call into the host. A
table is data: it can be listed, shared and tested without running the host.
**Rejected: copying a library into each execution.** A thousand requests would
copy a thousand tables to read the same members.

### Resolution

`use a.b.c as d;` splits the path on dots and looks the first name up in order:
the execution's library (`gltang_execution_set_libraries`), the program's
(`gltang_program_set_libraries`), then the built-ins. The first match wins, so a
host can shadow `math`. The rest of the path is walked with the engine's attribute
rule (reference 9.1 and 10.2): a library's member, `Not implemented` for an absent
one, and the same rule on whatever a member is (`use math.pi.x as q` is
`Not implemented`). A first name nothing provides binds null and the program
continues, and so does a dotted path through it - ctang binds the error
`Not implemented` there, which is ledger row D-012. `use a; a = 42;` replaces the
variable only: the library is not written.

The **program layer** exists so a host can ship a template together with the
libraries it was written against. `gltang_program_set_libraries` is allowed only
while the program has one reference, so a program that several contexts share is
never written (AD-22); a template registered in a library holds a reference, which
counts. A template called from a page uses its **own** program's layer, then the
execution's: the layers are those of the program whose frame is running, found
through the frame's program index.

A **factory** is called when a `use` that reaches it executes, once for each
execution of that `use`, never at registration, and makes a scalar (null, boolean,
integer, float or string); false is "not available" and binds null. A factory
reached through an attribute (`u.name`, where `u` is a library bound earlier) is
called each time it is read. **Rejected: calling factories at registration.** The
host would pay for a user's lookup on every request whether the template used it.

### The callback contract

A native function or a factory is an **opaque host function** (AD-23). It runs
synchronously, on the context's owner thread, inside the run. It is given a call
object (the argument count and accessors that give a kind, a boolean, an integer, a
float and text; setters that answer with null, a boolean, an integer, a float, a
tagged string or an error kind) and its `user` pointer, and nothing else. It
cannot pause, because it has no frames of its own to save, and it cannot call back
into guest code. Anything it calls on the execution that is running it is refused:
every `gltang_execution_*` mutator returns `GLTANG_ERR_INVALID` (the execution is
`in_host`, and a started execution refuses its setters anyway), `grcore_run` and
`grcore_resume` return `GRCORE_ERR_INVALID` because runtime-core sees a running
context. `tests/unit/test_library.cpp` has a native and a factory that do all of it
and a run that is unaffected. This is the contract story 9 deferred when it said
"the `use` resolver is a host callback with no re-entrancy or thread rule".

A native costs one unit of fuel like any call, and one more per
`GLTANG_WORK_BYTES_PER_FUEL` bytes it returns. The call object points into the
operand stack - copying the arguments would cost an allocation per call - which
is sound because the function cannot reach a GC point or push a frame. A string
answer is copied when it is set, since the function's buffer may be gone when it
returns. Returning false with nothing set is the error `Host function failed`;
returning with nothing set is null.

**Rejected: letting a native call guest functions.** It would make the native a
nested activation that cannot pause, and a pause inside it would have to be
refused by the poll (AD-5), which is what AD-23 states instead.

### Native values

A native function, a template, a library and a generator are heap objects whose
kinds are `GLTANG_KIND_FUNCTION` (the first two), `GLTANG_KIND_LIBRARY` and
`GLTANG_KIND_RNG`. They are not new Tang types: they follow the rules of a value
the language has no operator for. A library or a generator is truthy, prints as
nothing under `print`, shows as `Library: math` or `RNG` inside a container, and
is `Not supported` for arithmetic, comparison (equality included), indexing,
slicing and every cast, `math.pi = 3` is `Not supported`, and calling one is
`Invalid function call`. Each of those was probed against ctang and agrees, with
one departure (D-021): ctang shows a native function inside a container as
`FunctionNative<0x55c4236505a0>(Callback<...>, BoundObject<...>)`, two pointers,
where lang-tang shows `Function(native)`.

A library object holds a pointer to the sealed `GLTANG_Library` and a native
or template object a pointer to its member; neither is traced, because the
execution keeps every library it can reach alive. A native bound to a value (a
generator's `set_seed`, which is `r.set_seed` evaluated and then called) holds
that value in a traced slot that is written only through the collector's barrier.
A generator is the `std::mt19937_64` state inline in a 2.5 KiB object, with no
handle to free.

### The seed sequence and the generator per context

A `GLTANG_SeedSequence` holds a master seed and an atomic counter. Draw `k` is
`splitmix64(master + k * 0x9E3779B97F4A7C15)` in the standard construction (the
state is advanced by the golden gamma, then mixed), so any thread may draw, two
sequences from one master give one series, and the first draw of master 0 is the
generator's well known first output (`tests/unit/test_random.cpp` checks both,
and that 8,000 draws from four threads are exactly the first 8,000). The master
seed is the host's, or operating-system entropy (`getrandom`, then
`/dev/urandom`, or `rand_s` on Windows). No generator is seeded from a clock.

Each execution has **one** `random.global`, made on first access with the next
seed of its sequence, and `random.default` is a **new** generator on every access,
seeded from the same sequence. `random.seeded(n)` is bit-exact with
`std::mt19937_64(n)` - the state is cutil's `gcu_random_mt64_*` - so `next_int` is
the next 64-bit word as a signed integer, `next_float` is
`(word >> 11) * 2^-53` and `next_bool` the low bit, and a negative `n` is its
two's complement. `set_seed(n)` reseeds and returns the generator, except on the
global, where it is the error `Cannot change the seed of the global random number
generator`. Arguments are checked in ctang's order: the count, then the kind, then
the global's refusal. With no sequence attached, an execution makes a private one
from entropy the first time it needs a seed. Two executions given one master seed
and one order of first use get the same numbers: the global's draw is the first
one drawn, so which of `global` and `default` is read first decides which takes
draw 0, which a test shows.

**Why it is in lang-tang.** runtime-core is the frame protocol and the context,
and a seed is a language library's business: AD-25 puts the policy in the group,
but the group has no slot for it yet, and a `GLTANG_SeedSequence` in a header that
includes only stable ones (`seeds.h`) is what the host needs today. Moving it is a
change to where the host puts a pointer. **Rejected: a clock-seeded `default`**
(ctang), which makes two runs of one program differ for no reason. **Rejected: one
process-wide generator**, which two contexts on two threads would race on.

### The error list

An error is a value, and the language lets one go nowhere. The error list is how
the host learns which. An error enters the list when it is **swallowed**:

1. `print` of an error whose rendering is nothing (a marker such as
   `[INTEGER TOO LARGE]` prints itself and is not swallowed);
2. an expression statement whose value is an error that no variable holds
   (`s[0] = "x";` on a string, `1 / 0;`, a call that returned one) - so the
   discarded error of reference 13.9 is listed, which is D-001;
3. the final value of a *called template*, when it is an error;
4. a budget scope's stop of a template call.

At the top level the last statement's value is the program's result, which the
host reads, and is not swallowed; it is listed only if a later statement replaces
it. A statement that only reads a variable or stores into one (`x = 1 / 0;`, `x;`)
keeps the error where a program can test it, and is not listed, which is the
"tested and handled" case. The compiler marks the rest: a `DISCARD` opcode in a
function body, and operand 1 of `SET_RESULT` at the top level.

Each entry says the error kind and its message, **how** it entered (printed,
discarded, template-result, scope-limit, created), the **template** it came from,
the **chain** of template calls above it (outermost first, each with the calling
template's name and the file and line of the call), and the origin's file, line,
function and offset. An error value cannot leave the activation that created it -
a template call yields text, or the limit error that the calling side makes - so
the chain where it is swallowed is the chain where it was created, and the origin
captured at creation is the one reported. Each error value is entered **at most
once** (a flag on the value), so `x = 1 / 0; print(x); print(x);` is one entry.

A host switch, `gltang_execution_set_log_all_errors`, enters **every** error at
creation instead, as `created`, and then the swallow rules add nothing for it,
since the flag is already set. The list is capped (`gltang_execution_set_error_limit`,
default 1,024), and entries past the cap are counted in
`gltang_execution_errors_dropped`. The entries and their chains are the
execution's, allocated with the context's allocator, so a long list is charged to
the context's memory budget; the strings they point to belong to the programs and
libraries the execution holds, so they are valid until it is destroyed.

**Why not "at creation".** Most errors are not problems. A program that compares a
result with a division by zero, stores an error to test it later, or branches on
one has handled it, and a list that held every one would bury the lost ones, which
are what a host must know about. Only the host knows whether a lost one was
allowed, so the list records swallows, and the switch records everything.
**Rejected: an exception-like channel from the engine.** Errors would stop being
values and guest code could not rely on the language's rule that they do not
unwind (AD-5).

### Halting on the first error

`gltang_execution_set_halt_on_error(true)` makes the first error value created end
the run. It is done through runtime-core's keyed request and poll handler, not by a
side channel: a request kind is defined for lang-tang's key
(`grcore_context_request_kind`), a DECIDE handler votes unwind and calls
`grcore_pollcall_set_unwind_result(GRCORE_ERR_GUEST)` while the request is
pending, and the error's creation posts the request through the context's port and
polls at once (the runtime poll), so that nothing more is printed. The entry is
recorded as `created`, the frames are popped, and `grcore_run` or `grcore_resume`
returns `GRCORE_ERR_GUEST` with the execution `UNWOUND`. The `Limit Exceeded` value of a stopped template call is entered as a
`scope-limit` and is subject to neither the halt option nor the log-everything
switch. A template call's scope does not catch it: the vote is the run's own (the scope is not the only voter, so
`grcore_budget_scope_exhausted` is false), and guest code cannot catch it (AD-5).
**Rejected: returning a flag from each operation.** Every operation would carry
the check, and a missed one would let the program go on.

### Template calls and budget scopes

`gltang_library_add_template(library, name, program, scope_fuel, policy)`
registers a compiled template; the library retains the program. `use sidebar;`
binds a function value and `sidebar()` is the call, which takes **no arguments**
(any argument is `Argument Count Mismatch`): a template gets its data from the
libraries the execution carries, which it shares with its caller.

It is a **guest-to-guest call**. The interpreter pushes the callee program's
top-level frame on the same guest stack and continues in the same loop, with no C
recursion and no host callback, so a pause inside it returns to the host, and
`grcore_resume` carries on in it, on any thread. The callee has its **own program
scope** (its own variables, fresh for each call, so a template may call itself
within the depth budget) and its **own output**: its prints go to a buffer of
typed segments of its own, and the call's value is that output as a string with
every segment's encoding tag kept, so `print(sidebar())` appends it tagged and
rendering encodes each. Template text outside tags is trusted, as ever. A
template does not return its last statement's value. **Rejected: returning a
flat, untagged string.** A sidebar's escaped text would be unescaped when it was
nested in a page, which is the vulnerability the tags exist to prevent.
**Rejected: a template call that re-enters the interpreter from C.** It would
need the nested-activation rule (it cannot pause to the host) and a C stack
frame per level, so a deep page would limit what the host's thread stack allows.

**Ordering at the boundary.** Before the callee frame is pushed the engine
flushes the pending fuel, so everything spent so far is charged to the caller's
scope, then opens a runtime-core scope (`grcore_budget_scope_open`, at the top of
the stack as it stands, so the scope's base is the caller) with the registration's
`scope_fuel` (exclusive; `GRCORE_UNLIMITED` is allowed, and is still under the
request ceiling) and the policy. When the callee reaches its `HALT` the engine
flushes again, so the fuel the callee spent is charged to its scope, pops the
frame, closes the scope, enters a final error value as `template-result`, and
pushes the string. The caller's clock stops while the callee runs (B does this),
and a charge is counted against the request's inclusive ceiling whatever scope is
open, so a page that calls 10,000 children is stopped by the ceiling (a pause,
then `GRCORE_ERR_LIMIT` when the host unwinds) while no child's own budget is
touched. The depth is the context's guest-depth budget, counted in frames across
templates and functions together, so a call past it is the error
`Recursion Limit Exceeded` as before.

**Policies.** `EMPTY` and `SEGMENTS` are runtime-core's unwind policy; `PAUSE` is
its pause policy. `EMPTY` and `SEGMENTS` record a `scope-limit` entry; `PAUSE`
records none, since a pause is not a stop.

- When a scope runs out and nothing else voted to stop (the ceiling, a terminate
  or memory did not), the poll unwinds and `grcore_budget_scope_exhausted` names
  the scope. The engine records a `scope-limit` entry (the template, the chain
  above it, and the file and line the callee was stopped at), calls
  `grcore_budget_scope_unwind`, releases whatever the callee had pinned (the
  execution's temporary roots back to where they were when the call began), and
  gives the call a value. `EMPTY`: the **limit error** `Limit Exceeded`, whose
  origin is the call site, flagged as entered already so a `print` of it does not
  enter a second entry. `SEGMENTS`: the output the callee had **finished printing**,
  as a string - the output records how much of it belongs to prints that
  completed, so the cut lands between whole prints and never inside one even when
  a huge print was stopped half way.
- The same unwind is reached from the **runtime poll inside a native operation**
  (string building, `array * int`, printing a large container): the operation
  returns the unwind sentinel, which reaches the same label in the interpreter as
  a poll instruction's unwind does, so a scoped stop takes the scoped path and
  not the whole-run one, and the half-built object is released.
- `PAUSE`, a development setting: the poll pauses at the callee's file and line
  with the fuel key; the host raises the innermost scope with
  `grcore_context_fuel_scope_set_budget(context, grcore_context_fuel_scope_top(context), ...)`
  and resumes, or terminates (`GRCORE_ERR_LIMIT`). A native cannot pause, so a
  scope with this policy that runs out inside a native operation is unwound as the
  whole run, with the limit.
- A limit that is **not** the scope's is never caught by it: the whole run pauses
  or unwinds as for any program, closing every scope with it.
- A scope exhaustion in the caller after the callee's scope has closed is the
  caller's own.

**The program index in frames and the poll identity.** A frame's function word
carries the program's index (into the execution's table of programs; 0 is the main
program) in its high 32 bits and the function index in its low 32; its flags word
carries its activation's depth. The engine descriptor's `locate`, `inspect` and
scope interface and the poll identity `(function word, offset)` decode both, so a
frame walk, a pause and the debugger name `(program, function, offset)` correctly
for every frame, and a frame reads the variables of its own activation. The
fp/sp/slot pointers and the activation pointers are reloaded after every GC point
exactly as before (the moving stack shows it), and everything per call lives in an
**activation record in the execution** - its result, its variables, its output,
its call site and its scope - which the collector scans as a root source, so a
paused context migrates with it. The program itself stays immutable and shared;
each execution keeps its own table of the programs it has run frames of, with
their constants made into heap values.

**Rejected: a separate context per template call.** It costs a context (a stack, a
heap, a group slot) per call, makes the caller's pause and the callee's two
things, and breaks the shared variables of a page. **Rejected: enforcing the
scope's fuel in the interpreter.** The scheduler already counts, stacks scopes and
decides the verdict (AD-21); the engine only knows where the call boundary is.

### Labels

`seeds.h` is the one new header that is `stable`: it includes only stable headers
(`core.h`) and is the C embedding API's host entry for the seed. `library.h` is
`free`, for a reason of the rule and not of caution: a stable header includes only
stable ones, `gltang_library_add_template` names `GLTANG_Program`, and the call
object names `GLTANG_ErrorKind` and `GLTANG_ValueKind`, all of which are free
(`program.h`, `value.h`). The error list is read through `execution.h`, which is
free for the same reason that it names the execution. Making the host API stable
means deciding to freeze those, and `tools/check-labels.sh` still fails an
unlabelled or doubly labelled header; two fixtures show that `library.h` labelled
stable and `seeds.h` labelled free are refused.

**`execution.h` stays `free` (decided with Corey, 2026-10-05, when story 15's
text called it `stable`).** Promoting it needs `value.h`, `program.h` and
`library.h` to be stable first, because it names their types, and none of them
is ready: calls in compiled code and floating point
(`planning/specs/spec-runtime-calls/`, `planning/specs/spec-runtime-float/`)
will change the JIT statistics (`GLTANG_JitStats`) and the execution options
this header carries, and a `stable` label would make each of those a breaking
change. The label follows the decision to freeze, and that decision comes after
those two specs.

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
with the node count, `reject`, ctang's `killed` - and then **runs every file on
both engines** and compares what they did (see "Verification" below). Both
comparisons are held against `documentation/divergence-ledger.md`.

- It **fails when ctang is absent, and never skips.** `ORACLE_PC` names the
  package, and a bogus one is how the gate is seen to fail.
- A divergence is accepted only if a `recorded` row names that corpus file.
- A `recorded` row that names a file that agrees in execution is **stale** and
  fails (the parse comparison does not judge staleness: a row names the files
  that diverge in execution, which parse the same).
- An unrecorded divergence fails, naming the file and the first difference.
- A runner that exits with a usage or read error, or prints something
  unreadable, is a failure of the harness and throws; only a signal or the
  wall clock is `killed`.

All of this is a pure function (`oracle::judge`) driven by planted cases in
`tests/unit/test_judge.cpp`, and the differential itself mutates a real
verdict and requires the judge to name the file.

The rows are the departures the spec lists: ctang's open section 13 items (9
and 13), the per-context generator, budgets and the pause and unwind outcomes,
error reporting, and everything the execution differential found (D-009 to
D-015, D-017, D-023 to D-030). The ledger is closed when no row is `open`, and
it is: its final state is in "Verification".

Two things the corpus found about ctang are worth recording here because they
are properties of the language and not of the port: `\r` is not whitespace, so
a CRLF source is a syntax error in both engines
(`script/reject-crlf-line-endings.tang`), and a digit sequence of four or more
digits after a backslash is a syntax error, not an octal escape followed by a
digit (`script/reject-octal-four-digits.tang`).

## Verification

Story 11 makes lang-tang's behaviour something that is *checked* and not merely
tested, and every instrument it adds is shown to fail on a defect it was meant
to catch. Five instruments, one proof that they work, and the ledger they feed.

### The execution comparison

`oracle_ctang run-script|run-template FILE` runs the file in ctang and prints
`refused`, or the rendered output (`gta_unicode_string_render`: every segment
encoded per its tag) and the final result as a **kind and a canonical text**:

- null, function, library, rng: the empty text; bool `true`/`false`; integer
  decimal; float `%.17g` of the double; string its bytes;
- an error is its **message** (a marker is its marker text) and never the file
  and line it came from, which ctang's errors do not carry (D-018);
- an array is its count and its elements one level down, each as kind and text,
  a container element being its kind and count; a map is its count only, because
  its key order is D-015 and a key list would turn every map result into a row;
- a result pointer that cannot be a heap object is the kind `garbage` (D-023).

`tests/oracle/run_lang_tang.h` gives lang-tang the same program with 200 million
fuel (a program it pauses on is `Paused`, which agrees with a ctang killed by the
wall clock, and with nothing else), the default libraries and no host extras,
and reduces its result by the same rule. `agree` compares the output bytes, the
result kind and the result text.

**Which ctang executor.** ctang has two: an x86-64 JIT and a bytecode virtual
machine, and requires them to agree. The runner uses the **virtual machine**
(`GTA_PROGRAM_FLAG_DISABLE_BINARY`, environment ignored) and never the JIT: the
JIT exists on one architecture, and it asserts while compiling
`script/tests-first-program.tang`, a program ctang's own parse tests contain,
where the virtual machine runs it. (The story says "never its bytecode engine or
JIT"; ctang has no third executor, so this is the reading that can be run, and
it is reported as such.)

**What is not compared**, and why: the error list (lang-tang's alone, D-006),
error origins (D-018), the order of map keys (D-015), and the rendered text of
`random.global` and `random.default` (D-003; `random.seeded` is compared and
agrees word for word). The wall clock for the ctang child is 2.5 seconds and its
address space is bounded at 2 GiB, because ctang allocates without end on a
program that ends in a function declaration (D-011).

**Rejected alternatives.** Running ctang in-process: it crashes, hangs and
asserts, and AD-2 and AD-16 keep it out of every library and tool. Comparing
`tang`'s stdout: it prints no result. Comparing the container renderings as
text: it makes D-013 and D-015 into noise on every map. A skip list for the
corpus: the test has one, with each entry's reason, and it is empty - the
reject-* files are run too (both engines must refuse them), the files the
compiler refuses are listed with the reason (`kCompileRefused`), and a file that
diverges is a ledger row and never a skip.

### The execution corpus

555 files under `tests/corpus/` (147 before): 485 that both engines run to the
end, 64 that both refuse, and six others: the three runaways (lang-tang pauses,
ctang is killed), the one program ctang cannot finish (D-011), and the two that
ctang refuses and lang-tang runs (D-026). It covers every operator and cast and their error kinds, strings (every
encoding, slices with every step, graphemes), arrays and maps, the four loops
with `break` and `continue`, functions, recursion to the depth budget, `use` of
`math` and `random.seeded`, every template construct with its segment encodings,
and a file or more for every recorded row. A file is refused by both engines
exactly when its name says so (`reject-*`) or it is on `kCompileRefused`.

### The differential fuzz run

`tests/fuzz/gen.h` builds a valid program from a seed with a seeded
`std::mt19937_64` (every choice by integer arithmetic, so a seed gives the same
program on every machine), in script or in template form, over the whole
language, bounded: a few kilobytes, loops of at most six passes nested three
deep, single recursion only, and every assignment inside a loop cut to a bounded
length. Its comment lists each avoided construct with its row id:
D-003/D-021 (`random.global`, native values), D-009 (a loop or an untaken `if` as
the last value: a program always ends in an expression statement), D-010 (every
function ends in a `return`), D-011 (functions are declared first), D-012,
D-013 (containers hold integers), D-014, D-015 (a map with more than one key is
never printed), D-017, D-023, D-024, D-025 (no global array in a function),
D-026 (`use` once, at the top), D-027 (arrays are compared with `==` only),
D-028 (no array is repeated or sliced), D-029 (the right operand of a string
`+` is `san(x)`, which turns an error into 0) and D-030 (a value stored into an
array element is `(e + 0)`).

`make test-oracle` runs the fixed batch (`FuzzDiff.FixedBatch`: seeds 1 to 220,
both modes, 440 programs) in 9 seconds; `make fuzz-diff FUZZ_DIFF_COUNT=N
FUZZ_DIFF_SEED=S` runs a campaign. A divergence prints its seed, mode and whole
program and the first byte at which the output or result differs; the test that
plants one requires the failure to name the seed. `tests/unit/test_gen.cpp` runs
the same programs on lang-tang alone (determinism, that every one compiles and
finishes, that the avoided constructs never appear, that the whole language is
reached), so the lang-tang side is also run under torture.

First measurements: a campaign of 12,000 programs (seeds 1 to 6,000, both modes; it includes the fixed batch's seeds) ran in 61
seconds with no divergence once the findings below were fixed or recorded. The first run of 440 had 35 divergences and 5 distinct causes.

**Rejected alternatives.** Random bytes through the parser (the libFuzzer
harnesses already do that, and almost nothing they produce runs); a generator
without types (nearly every program would be one error); one ctang process per
batch (a crash or hang would lose the batch's verdicts).

### Divergences found, and what became of each

- **Fixed in lang-tang** (with a test): a slice part held in a variable that is
  null was `Invalid index`, where ctang, whose parser pushes a null for every
  omitted part, takes any null as omitted (fuzz seed 8; `Slice.AVariableHolding
  NullIsAnOmittedPartAsInCtang`). Reading a host string whose bytes are plain
  ASCII charged no fuel for the copy, so a loop that read a megabyte string paid
  a few units per megabyte (found by the native gate;
  `Natives.ReadingAHugeHostStringInALoopIsChargedForEachCopy`).
- **Recorded, found by the corpus:** D-009 to D-011 (what a loop, an untaken `if`
  or a function without `return` evaluates to, and a trailing function
  declaration, on which ctang's virtual machine does not terminate), D-012
  (rewritten), D-013 (widened to `print`), D-014 (corrected: array `*` copies in
  ctang, only `+` aliases), D-015, D-017, and the new D-023 (a program ending in
  `use` leaves a garbage result in ctang), D-024 (a parameter named like its
  function), D-025 (stores into a global array from a recursive function) and
  D-026 (where `use` may appear).
- **Recorded, found by the fuzz run:** D-027 (`!=` on arrays is `==`), D-028
  (arithmetic on an element of an array that `*` or a slice built is done in
  place: `a = [5] * 2; x = a[0] - 1;` leaves `a` as `[4, 5]`) and D-029 (a string
  plus the error `Not implemented` is `Not supported`) and D-030 (the same
  in-place arithmetic, reached by an index store: `x = 5 + 1; a[2] = x;
  j = 20 - a[2];` leaves `a[2]` as 14; found by the 900,000-program soak).
- **Not defects, noted:** a ctang that takes 2.5 seconds for `[1] * 100000000`
  (the early seed 67) is slow and not wrong, and the generator no longer repeats
  arrays; ctang's JIT asserts on `tests-first-program.tang` (see above).

### The frame-differential observer

`tests/observer.h` registers an OBSERVE handler (runtime-core's observe key) and
a request kind that is posted once and stays pending, so that every poll takes
the slow path and the handler runs at each of them. Per poll it records the
verdict and, for each frame of the **abstract frame walk and nothing else**, the
depth, engine, poll identity, file and line, slot count, each slot's kind and
inspected text, and each scope's kind and name and its variables' names and
inspected text. It compares two traces poll by poll and reports the first
divergence with its poll index and frame. Heap addresses are never compared:
only the inspected text of a value slot, and the header words (function, pc, sp,
depth) as the numbers the inspector prints, which are the same in every
configuration. It reads no engine structure, so story 15 used it unchanged for
interpreter against JIT (below) and story 12 can use it for a debugger attached
against absent.

`tests/unit/test_observer.cpp` runs 76 programs - forty script and twelve
template corpus files, twelve generated programs (six seeds, both modes), twelve sites (pauses inside calls, three-deep template
calls, scopes of each policy exhausted by a loop and by a native, errors across
a boundary, 40 children), all pausing and resuming - four ways: plain,
torture+verify, a stack that moves at every push, and phase-shuffled (AD-5). The
traces, the output, the result and the error list must be equal (2,290 recorded
polls and 3,552 pauses per configuration), and all four instruments together are
checked on the sites.

**Interpreter against JIT (story 15).** The same 76 programs run six more ways:
every function compiled at its first poll (`jit threshold 1`), the same under
torture and verify, the same on a moving stack, the same with the poll phases
shuffled (tier-up is an ACT handler), and with statement polls on
(where a compiled `LINE` polls too), alone and on a moving stack. Each trace must
equal the interpreter's at every poll - the same polls, the same slots, the same
header words, the same pauses - and so must the output, the result and the error
list. A pause at a compiled poll is the interpreter's pause, because the poll
helper wrote the frame first. The run is not vacuous, which the test checks:
compiled code was entered over a hundred times, took the slow path over a hundred
times where the observer was watching, and paused the run. A poll costs about a millisecond to record (the globals of
every frame are read), so a generated program records its first 150 polls and
compares the count of the rest.

It is seen to fail: a planted slot mismatch, a missing poll, a different depth, a
changed variable and a changed line are each reported with the poll and the
frame; an order-dependent DECIDE pair (B pauses unless A already ran) is caught
by the shuffled run at the verdict of the first poll where B ran first, and an
order-independent pair is not.

### The native budget gate

`src/vm/natives.def` is the engine's list of natives, 17 entries: 14 whose work
the guest controls (`ARRAY_GROW`, `DEEP_COPY`, `EQUALITY`, `ARRAY_CONCAT`,
`ARRAY_REPEAT`, `ARRAY_SLICE`, `STRING_CONCAT`, `STRING_SUBSTRING`,
`STRING_SLICE`, `STRING_RETAG`, `STRING_RENDER`, `STRING_FROM_UTF8`, `PRINT`,
`RENDER_TO_STRING`) and 3 verdict polls (`ALLOCATION_REFUSED`, `CALL_REFUSED`,
`HALT_REQUEST`: one poll, no work). Every poll in the sources is
`GLTANG_NATIVE_POLL(exec, NAME, work)` or a pacer made with
`GLTANG_PACER(exec, NAME)`, and a name that is not in the list does not compile.

`tests/unit/test_native_gate.cpp` reads the list and the sources and fails the
build if a poll names no entry, if an entry is named by nothing, or if an
unbounded entry has no row in the table - so a new native without a gate row is
a failing build. Each row has a **build** (makes the operand; its fuel and bytes
are the baseline, measured first on its own) and an **op** (the adversarial
operation, on a budget of its own beyond the build): 27 rows (26 under the instruments), at least one per
unbounded native, from `s = "x"; while (true) { s = s + s; }` under 1,000 fuel to
repeating an array into 8 GB under a 4 MiB memory budget. A row requires an
outcome that is a verdict (paused, unwound with `GRCORE_ERR_LIMIT`, or the
program's own error value) and bounds **the work done**, measured by counters
the test owns: the fuel the context charged (budget plus a slack of 1,500, a few
poll chunks) and the most bytes the group held at once, counted by the allocator
and page provider the group was given (`tt::Tracker`), against the largest
allocation the operation legitimately makes. A memory row also requires a
collection to have run before the verdict (`grheap_stats.collections`). The wall
clock is only a backstop (an `alarm` kills a hung case). The one native with a
slack of its own is `STRING_FROM_UTF8`: the copy and scan of a host string are
single library calls, so the whole charge (1 MiB / 64 = 16,384) is made at once
before any of the work, and the verdict follows it.

Under the collector's torture mode and a moving stack the operands are smaller
(32 KiB of text, 20,000 elements, a 32 KiB host string) and the one row that
builds a container 2,200 deep, which copies it at every level, is left out
(`kSmall`): there every allocation is a collection, and it runs in the plain
build.

Each limit has its own row: **fuel** (an endless loop), **memory** (a string that
doubles forever, and a list or a map built in an endless loop, each under a
small budget: the budget refuses the growth after a collection has run, and the
fuel ends the loop), **guest depth** (past the budget: the `Recursion Limit
Exceeded` value; with no budget at all: the fuel ends it, with the frames on
the guest stack and the C stack untouched) and **native depth** (containers
nested past 2,048: the error value in copy, equality and print). The ones that
need the host are separate tests: the **wall clock** (a timer thread posts the
request while a long native runs: an unwind, or a pause for a loop), a **page of
10,000 children** under the request budget (a pause, then `GRCORE_ERR_LIMIT`,
and no child stopped by its own scope), and a native inside a scope.

### Planted defects in the library

`tools/check-planted.sh` builds a throwaway copy of the library under
`build/planted/` (nothing in the working tree is touched), applies one patch from
`tests/planted/` at a time with `patch --fuzz=0` (it checks that the patch
matched and that a file changed, so a patch that applies to nothing fails the
script), builds the test named for it and requires it to **fail**, then removes
the patch and requires the same test to **pass** (the control). `--selftest`
shows the script fails on a patch that matches nothing and reports one that
breaks nothing as not caught. Observed:

| Patch | Defect | Instrument | The failing line observed |
| --- | --- | --- | --- |
| 01 missing-root | the execution's temporaries are not reported to the collector | torture+verify over `testExecute_complex` | `barrier-verify: the holder is not a heap object: object type "(none)"`, abort |
| 02 missing-gc-store | an array element stored with a plain assignment | barrier-verify, the same | `barrier-verify: a slot was written without grheap_store: object type "lang-tang array storage"`, abort |
| 03 order-dependent-decide | two DECIDE handlers whose combined vote depends on their order | `testObserver` | `script/arithmetic.tang: plain against phase-shuffled: poll 0: verdict: continue against pause` |
| 04 native-never-polls | string concatenation without its polls | `testNative_gate` | `bytes held 25310280 against a build that held 135360 and a bound of 2097152 more` |
| 05 frame-slot-mismatch | a value slot's text gains a character under torture | `testObserver` | `script/arithmetic.tang: plain against torture+verify: poll 0, frame 0: slot 4 text: null against null!` |
| 06 wrong-operator | integer `*` answers one too many | `testOracle` | `unrecorded divergence: script/array-built-in-a-loop.tang` |
| 07 silent-runner | the oracle runner exits 0 and prints nothing | `testOracle` | `the oracle runner printed something unreadable for .../arithmetic.tang` |
| 08 jit-wrong-tag | a compiled `ADD` or `SUB` tags its result as a function value | the frame differential, interpreter against JIT (`testObserver`) | `generated/1-template: plain against jit threshold 1: poll 37, frame 0: scope program variable ...` |
| 09 jit-skipped-fuel | a compiled `LOAD_LOCAL` is not charged | the fuel-parity test (`testJit`) | the two runs' fuel totals and pauses differ |
| 10 jit-missed-write-back | the guest frame is not copied back into the compiled frame after a poll | the write-back test (`testJit`): a poll handler overwrites a local, as a moving collector would | the two runs print different sums |
| 11 host-pointer-no-hook | the template type has no snapshot hook, so its payload keeps the address of the library member | the address scan of every blob of a snapshot (`testSnapshot`) | `an address is in the runtime-heap blob` |
| 12 skipped-output-capture | the snapshot is written as if the output so far were empty | the output-so-far test and the corpus sweep (`testSnapshot`) | the restored output starts from nothing, and the sweep names the first program whose output differs |
| 13 temporaries-through-a-copy | the execution reports each temporary root through a copy, so it is visited and never updated | the relocation arm: `testExecute_simple` under `GRHEAP_RELOCATE=1` with torture (and, with relocation off, the same build passes) | a poisoned read in an array operation, or a result that differs |
| 14 array-storage-through-a-copy | an array's trace function reports its storage pointer through a copy | the same | the same |

`make test` runs 03 to 12 (`check-planted-quick`, about two minutes with the
first build of the copy); 01 and 02 are part of `make test-torture`
(`check-planted-slow`, 6 seconds once the copy is built). `make check-planted`
runs all twelve. 13 and 14 are run by `make check-planted-relocate`, which the
relocation arm (below) calls: they need a runtime-heap that moves objects, and
the script also runs each caught case with torture and without relocation,
where it must pass, which is what shows relocation to be the instrument.

### Relocation: the engine against a heap that moves

Everything above runs against a collector that never moves an object, so it
cannot say whether a reference the engine hands the collector is one the
collector may *rewrite* (AD-12). runtime-heap has a test-only build option for
that (`RELOCATE=yes`; its `design.md`, "Relocation torture"): every collection
moves every unpinned object to a new cell, rewrites every reference it was shown
and poisons the old cell, so a value or a pointer into an object that is kept in
a C variable across a GC point reads poison. `make test-relocate
RELOCATE_PREFIX=<prefix>` is the arm that runs this library against it; it
takes the prefix that heap was installed in (a private one, under the same
package name), and with that named `make test` runs it too.

**The arm.** It builds in a tree of its own (`release-reloc`) against that
prefix and first runs `tools/check-relocation-required.sh`, which asks the
installed library whether it has the mode and shows a collection moving an
object, and **fails, never skips,** when it does not (run against a normal heap
it exits 1, naming it). Then every unit suite runs with `GRHEAP_RELOCATE=1`, and
the engine-driving suites (`TORTURE_BOUNDED`) again with torture and verify, so
that a move happens at every allocation and every poll; with the JIT built, the
same again with every function compiled at its first poll. `testRelocate` (built
in this arm only) runs a set of programs on an ordinary heap and on a relocating
one and requires the same result, output and error list, and that objects really
moved. Last, `check-planted-relocate` plants two defects (13 and 14, a
reference visited and not updated) and requires each to be caught.

**What it found, and the rule that followed.** The engine was written for a heap
that does not move, and its rule (`vm_internal.h`) said an object reachable from
a root could be kept in a C variable. It cannot: a collection rewrites the root
and not the variable. Every operation that allocates or polls more than once, or
loops over a string or an array with a poll in the loop, held a value or a pointer
into an object across a GC point and read it afterwards. Fixed, one commit for
each family, each with a test in `testRelocate`: growing an array or a map
(`array_new`, `array_grow`, `map_new`, `map_set`, and the stores that call them:
a value stored beyond the end of an array that is itself the value, a boxed
integer stored while the array grows); the deep copy and the comparison of
containers; the operators that build arrays (concatenation, repetition, slicing,
`+` with a string); every string operation that copies in chunks (concatenation,
substring, slice, retag, render); the sinks (printing and rendering a container
or a string into the output, which can only move at a poll, and a poll collects
only when the memory budget asks it to reclaim, so no test of the arm reaches
that change: it follows the rule and is read, not run); and a native bound to a
value. The rule is now: a value held across a GC point is pushed as a
temporary (`gltang_vm_temp_push`) and read again from it (`gltang_vm_temp_at`),
and a pointer into an object is derived again after every poll. The interpreter's
own loop already did this (it reloads the frame after every GC point) and the
baseline JIT's poll copies the guest frame back after the collector has rewritten
it, so neither needed a change. No address is used as a key anywhere in the
engine.

**Limits.** The arm proves the paths the suites run; a path no suite reaches is
not shown. A test that keeps an address in a C variable across a run (the
retention tests, which name an object by its address) turns relocation off for
its context (`Config::relocate`); everything else runs moved.

### What runs where

- `make test`: every unit suite; the engine suites, the generated batch, the
  frame observer, the native gate and the corpus run of lang-tang alone again
  under `GRHEAP_TORTURE=1 GRHEAP_VERIFY=1` and again with
  `GLTANG_TEST_MOVING_STACK=1` (`TORTURE_BOUNDED`); with the JIT built, those
  suites once more with `GLTANG_TEST_JIT_THRESHOLD=1` (every harness-made
  execution tiers up at its first poll), alone, on a moving stack and under
  torture and verify; the oracle differential (parse, execution, the fixed fuzz
  batch); the quick planted defects; and the interpreter-only arm (`make
  test-nojit`: `JIT=no` in a tree of its own, the whole unit suite, the CLI test,
  the examples and the gates that apply). `make test-nodebug` is the same idea for
  the debugger: the library and its unit suites built with `WITH_DEBUG=no` in a
  tree of their own, against a copy of the prefix from which the `runtime-debug`
  and `text` `.pc` files are removed (and it fails if the debugger is still
  visible), so that nothing in the library or its tests can come to need either
  without a gate saying so; it is not part of `make test` because it needs the
  prefix named.
- `make test-torture`: **every** unit suite (`TORTURE_SUITES`, no exclusions)
  under ASan+UBSan with torture, verify and a moving stack, then the two torture
  planted defects. The oracle differential is not run under torture: the child
  ctang is not the subject, and the lang-tang side of the corpus is
  `testExec_corpus`, which is.
- `make test-tsan`, `test-asan`, `test-valgrind-quiet`: every unit suite (the
  new ones included), as before. Where each operation costs ten or fifty times
  more (the collector's torture mode, a moving stack, Valgrind) the new suites
  scale their work down and say how: `tt::heavy_instruments()` in
  `tests/exec_harness.h` is true there, the corpus run gives each file 20,000
  fuel instead of two million and leaves out three files that build containers
  thousands deep, the native gate uses 32 KiB strings and 20,000-element arrays
  and leaves out the 2,200-deep row, the generated batch runs 60 seeds, and
  under Valgrind the observer records each program's first eight polls and
  compares the count of the rest (its 76 programs still pause and resume 3,552
  times) and the wall-clock test with a real timer thread is skipped, because
  Valgrind does not wake a sleeping thread while another spins; the same test
  with the request posted from inside the run, which needs no scheduler, runs
  everywhere. Observed before story 15: `make test-valgrind-quiet` about four minutes,
  `make test-torture` about a minute and a half, `make test -j8` from an empty tree
  2 minutes 27 seconds (3 minutes 54 seconds serial). With the JIT (the observer
  runs 13 configurations of each program now, and the suites run again with
  `GLTANG_TEST_JIT_THRESHOLD=1`, and the second arm is built and run): `make test
  -j8` from an empty tree 5 minutes 27 seconds and serial 8 minutes 10 seconds,
  both arms; `make test-valgrind-quiet` 11 minutes (507 tests).

### The ledger, final

29 rows: **26 recorded, 3 fixed, 0 open.** The ledger is closed. Categories of
the recorded rows: 17 ctang defects, 5 limits, 3 error-reporting, 1 rng. ctang
stays in `libraries.txt` and the oracle gate: whether it still needs to be the
oracle is a later decision (AD-16 retirement).

### What is not done

No long libFuzzer campaign: the harnesses build with clang (`make fuzz-parse`),
and the library, its tests and `test-asan` build and run under clang (the clang
fixes are in `d273366`), but nothing has been fuzzed for hours, and the
12,000-program differential is a measurement, not a long campaign. No outside test suite (Test262 and the like). No debugger-attached
comparison: the observer is built for it and it is story 12. (The
interpreter-against-JIT differential is story 15's, above.) The execution corpus does not compare the order
of a map's keys or the error list, which have no ctang equivalent.

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

The host API is swept the same way, one level up: the harness's tracker serves a
context's allocator and page provider, and fails the Nth call. Each of library
resolution and a native call, a generator, a template call, a nested call with a
scope exhaustion and error-list entries, a scope stopped inside a native
operation, and the log-everything switch is run with every allocation failed in
turn, until a run in which none was. The requirement is an answer (a correct
result, the `Out of memory` value, or `ERR_OOM`), no crash and no live block
after the context is gone, and the run in which no allocation failed must equal
the baseline's. The library and the seed sequence, which are plain cutil memory
built before any context, are swept with the wrapped `malloc` of the parser's
sweep.

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

The library links `cutil`, `unicode`, `runtime-core` and `runtime-heap`, and,
built with the JIT (`JIT=yes`, the default), `runtime-jit`, and nothing else; the
`.pc` requires them (the last only for `JIT=yes`), and the manifest lists them, so
a bootstrap builds them first. `readelf -d` on the shared object shows exactly
those, never ctang and never the debugger. Built with `JIT=no` the library
includes and links nothing of `runtime-jit`, `src/jit/` is not compiled, and the
tree is `build/<os>/release-nojit`, so the two arms never share an object.

Two programs are **hosts of the debugger** (story 13): `src/tang.c`, the `tang`
command, and `examples/web_server.c`. A host is where a debugger is attached and
a DAP session is served, so these two, and nothing else, include
`runtime-debug`, and the `tang` and `web_server` binaries link it and `text`
(which it requires, and with it `chron` and `regex`). The Makefile finds
`ghoti.io-runtime-debug` and `ghoti.io-text` by pkg-config for those two only
(`WITH_DEBUG ?= yes`; a hard error naming the fix if missing). They are in
neither `INCLUDE` nor `DEP_LIBS`, so the shared and static library are linked
without them. `WITH_DEBUG=no` builds the library (`make all`) and individual unit-test
binaries on a machine without them, and the two programs then refuse to build, by name, so that nobody
runs a `tang` that silently lacks `--dap`; `tests/unit/test_tang_dap.cpp`, which
drives the real command, is the one suite left out in that case, and the build
says so. `make test` needs the hosts (`check-edges`, `examples` and `cli-test`
are among its gates), so it fails under `WITH_DEBUG=no`; run the test binaries
you want directly. The manifest (`suite/libraries.txt`) lists `runtime-debug` and `text` as
dependencies of `lang-tang` for the same reason: a bootstrap must build them
first, although only two programs use them.

`tools/check-edges.sh` enforces AD-2 over `src/`, `include/`, the generator
inputs, `bench/` and `examples/`, and over the NEEDED list of the shared library,
the `tang` command and every example. The rule is an allowlist: `cutil`,
`unicode`, `runtime-core`, `runtime-heap`, `lang-tang`, and, for the two host
files by path (includes) and the two host programs by name (NEEDED),
`runtime-debug`, `text`, `chron` and `regex`. `runtime-jit` is the one
conditional edge (`GLTANG_EDGES_JIT`, set by the Makefile from `JIT`): with
`JIT=yes` only the files under `src/jit/` may include it and the shared object and
the programs that link it may have it in NEEDED; with `JIT=no` no include and no
NEEDED entry of it is allowed anywhere. Anything else - ctang, another engine, or
the debugger anywhere but those two hosts - is an edge, and so is any include of
`binary.h`. The allowance is by file, not by directory: a host
name in the wrong directory (`examples/tang.c`), a second example, a library
source or a public header including the debugger fails the gate, and
`tools/check-gates.sh` shows each of them failing and the two hosts passing
(planted fixtures under `tests/gates/edges/`, and stub programs for the NEEDED
check), and shows the JIT edge allowed in `src/jit/` under `JIT=yes`, refused
anywhere else under it, and refused everywhere under `JIT=no`, and that a Makefile
whose stamps omit `$(JIT)` fails `check-stamps`. `tests/` is not scanned: it is the one place ctang may be included, and
a test runner is a host too - `test_tang_dap.cpp` links nothing of the debugger
but could - so no test binary is named to this gate.

`tools/check-labels.sh` classifies each header by name: the C embedding API
(`core.h`, `parse.h`, `libver.h`, `macros.h`, `namespace.h`, `allocator.h`,
`seeds.h`, the umbrella) is `stable`; everything under `ast/`, the headers the AST
is built on, and the engine's (`bytecode.h`, `program.h`, `compile.h`, `value.h`,
`execution.h`, `library.h`) are `free`: they are new, and a header joins the stable set by a
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

The host API's switches: `--seed N` is the master seed of the run's generators
(two runs with one seed draw one sequence; without it the seed is entropy),
`--log-errors` enters every error at creation, `--halt-on-error` ends the run at
the first one (exit 8, the run's `GRCORE_ERR_GUEST`), and `--errors` writes the
error list to stderr after the run, one `template:file:line: message` an entry
(the main program is named `main`) with the chain of template calls above it
indented under it as `in template:file:line`. A value that is not a number is a
usage error, exit 2. `tests/cli-test.sh` has a case for each.

`--dap` (story 13) makes the command a host of runtime-debug. It enables
statement polls, attaches a debugger and serves a DAP session over
`grdbg_transport_create_fd(0, 1)`: the host loop of `runtime-debug`'s
`examples/dap_session.c` (serve until `configurationDone`, run, and at every
pause notify, serve and resume). The DAP stream owns stdout, so the run's
rendered output goes to **stderr**, and the source must come from a file or
`--evaluate` (stdin carries the session); `--dap` with `--tree` or with stdin as
the source is a usage error (2), and so is `--dap` in a command built without the
debugger. The breakpoint `source` is the file name exactly as given on the
command line. A client's `terminate` ends the run with status 6; `disconnect`, or
a client that closes the stream, disarms the debugger and the run finishes free
(the exit status is the plain run's). A pause that is not the debugger's, a
`--fuel` budget, is shown to the client once (`stopped`, reason `pause`,
description `paused by fuel`), and the next `continue` unwinds the run with status
6: the command has no policy for raising a budget. `--jit-threshold N` (story 15) sets the polls a function makes before the
baseline JIT compiles it (0 is never; a command built without the JIT refuses the
option as a usage error), and `--jit-stats` writes one line of the JIT's counters
to stderr after the run, the way `--errors` writes the error list. A scripted
session against the real binary is `tests/unit/test_tang_dap.cpp`; it also shows that a debugged run
with every stop continued, or with breakpoints never reached, prints on stderr
exactly what the plain command prints on stdout, with the same status.

**VS Code** (`editors/vscode`, story 19). A debug adapter in VS Code is a
descriptor, and the descriptor here is the whole contribution: `extension.js`
returns `DebugAdapterExecutable("tang", ["--dap", file])` for a "tang" launch
configuration, which runs the file as a template, and
`DebugAdapterExecutable("tang", ["--script", "--dap", file])` when the
configuration says `"script": true`. `package.json` contributes the `tang`
debugger, its launch attributes (`program`, and `script`, a boolean that
defaults to false) and breakpoints in `.tang` files. No debugger is written in
JavaScript; the adapter is the command above. Nothing in the suite can start VS
Code, so two checks stand in, and each says only what it checks. The test,
`TheVsCodeExtensionsCommandLinesStartTheRealCommandForATemplateAndAScript` in
`test_tang_dap.cpp`, reads both command lines out of `extension.js`, maps the
command name to the command under test and drives `configure`, `stopped`,
`disconnect` over each, so a command line the real command does not accept fails
it; of the manifest it reads only that a tang debugger and a `script` attribute
are named. `make check-vscode` checks the manifest's structure (valid JSON, the
debugger, its required `program`, the `script` attribute and its default,
initial configurations, breakpoints, `main`) and the shape of the calls in
`extension.js`, and is seen to fail on a planted defect for each check
(`tests/gates/vscode`, run by `check-gates`). Neither compares the manifest with
the command line beyond that. That stepping a template in VS Code itself works
is a manual step and the closing checkpoint of story 19.

## The web-server example

`examples/web_server.c` is the milestone's success signal in one program, and a
demonstration, not a product. A plain accept loop on one thread, bound to
`127.0.0.1` only, `Connection: close`, no request bodies, no TLS; the request
line is capped at 4 KiB and the headers at 16 KiB. `GET /<name>` runs
`<templates-dir>/<name>.tang` (the name is `[a-z0-9_-]+`, anything else is a 404,
so there is no path traversal) in **a fresh context, heap and execution made for
that request** from one shared group, and destroyed after it. The templates
`sidebar` and `layout` are registered in one shared, sealed library with budget
scopes of their own, and `page` calls both. Each template is compiled with the
path the host read it from as its `file`, so a pause or a breakpoint names the
real file.

The policy for a pause is the example's, not the library's: a request has a fuel
budget (`--fuel`, default 200,000); the first pause raises it once (`--raise`,
default ten times the budget) and resumes; a second pause is final. The context
is terminated, and the response is `503` with `paused at <file>:<line>` read from
the second pause, so `/slow` (which needs the raise) finishes with `200` and
`/runaway` is stopped at its loop's line; the host process survives every case.
`200` carries `X-Template-Errors` (the length of the error list: `/broken` is a
`200` with one), `X-Context-Id` and, if the budget was raised,
`X-Fuel-Raised: 1`.

`/<name>?debug=1` makes the host accept one connection on a second loopback
listener (`--debug-port`, bounded wait `--debug-wait-ms`, then `504 no debugger
connected`), turn on statement polls, attach a debugger and run the host loop
over that connection. A budget pause during a debugged request is shown to the
client first and then handled by the same policy. A client that closes the
connection at a stop detaches (the request finishes free, `200`), one that sends
`terminate` gets `503 terminated by the debugger`, and the HTTP response is
written when the run finishes, with the body an undebugged request would have
had. The library never listens or accepts: this example does, on loopback, and
that is a way to show a debug session, **not a policy for debugging a server over
a network** - remote debugging over a socket as a product feature is a non-goal
of the milestone.

`web_server --self-test` (run by `make examples`) binds both listeners to
ephemeral ports, plays an HTTP client and a hand-framed DAP client from other
threads, and exits 0 only if every behaviour above holds (31 checks), including
a scripted session against `/page?debug=1` that stops at the breakpoint on
`side = sidebar();`, reads the scopes and variables, steps in (two frames, in
`sidebar.tang`), over, and out, and continues to `terminated`. Every wait in it is
bounded, with an `alarm` as the backstop. It runs from `make examples` and is not
part of the sanitizer trees, which build the unit tests only; under Valgrind it
is clean.

## The baseline JIT

A small baseline JIT (story 15, CAP-10, AD-9) behind a build option, `JIT=yes|no`.
It tiers up a function that is hot, compiles the part of it that is cheap and
exact to machine code through `runtime-jit`, and leaves compiled code for the
interpreter, in the same guest frame, whenever it meets anything else. It is the
first client of `runtime-jit` and of the two things `runtime-core` gained for it
(`a/code.h`, reference-counted compiled code, and `a/deopt.h`, a reader and writer
of a native frame by its metadata). `lang-tang`'s observable behaviour is the
same on both tiers, by construction and by test: no divergence-ledger row exists
for the JIT, and none may be added.

### How it works

**Hotness.** `POLL` bumps one counter per function in a per-execution table
(AD-22: feedback lives with the context). The main program's top level is counted at its entry poll only: it is entered once, so a crossing in one of its loops would compile code nothing can enter. At the threshold (default
`GLTANG_JIT_DEFAULT_THRESHOLD`, 200 polls; `gltang_execution_set_jit_threshold`,
`tang --jit-threshold N`; 0 is never) the function is queued and a request of the
engine's own kind is posted, so that the poll being made runs the **tier-up
handler**, registered by key (AD-19) in phase ACT (AD-5). The handler acts only
when the verdict is continue; on a pause or an unwind it does nothing and the
request stays pending. It compiles every queued function synchronously, allocates
nothing in the GC heap, and clears the request. The registration is made when the
execution is created (a registration is refused while the context runs), and a
failure to make it is not a failure to create the execution: the threshold is then
0 and the interpreter runs alone.

**Entry.** Compiled code is entered only right after a function's entry `POLL`
(bytecode offset 0) has returned continue in the interpreter: the compiled
function starts "after the entry poll", at index 1 with an empty operand stack and
the frame's locals as its parameters. The guest frame was pushed by the interpreter
exactly as for any call, and it is the frame a pause shows. Entering records a
`GRCORE_ACTIVATION_JIT` activation, which draws on the native-depth budget (AD-21):
if the budget is full the function is simply not entered this time, the interpreter
runs it and the budget is untouched. Each entry retains the code for the call, so
a function discarded during a run is freed only after the call ends. The tier-up
that compiled the function in this very poll is entered in the same invocation.

**What is compiled.** Inline: `POLL`, `LINE`, `POP`, `DUP`, `NULL`, `TRUE`,
`FALSE`, `CONST` (a small integer only), `LOAD_LOCAL`, `STORE_LOCAL`, `NEG`,
`NOT`, `ADD`, `SUB`, `MUL`, `LT`, `LE`, `GT`, `GE`, `EQ`, `NE`, `JMP`, `JMP_FALSE`,
`JMP_TRUE`, `AND`, `OR` and `RET`. Each step mirrors the interpreter's own inline
path: both operands carry the integer tag or a guard fails; `ADD`, `SUB` and `NEG`
check the result against the small-integer range; `MUL` needs both untagged
operands inside -2^29..2^29-1 so the product cannot leave the range; the operand
of `NOT`, the jumps, `AND` and `OR` must be a boolean. Everything else (`DIV`,
`MOD`, `CALL`, globals, casts, indexing, attributes, slices, the `SET_*`, `ADOPT`,
`ARRAY`, `MAP`, `PRINT*`, `ITER_*`, `DISCARD`, `SET_RESULT`, `USE`, `FUNC`, `HALT`
and any other constant) is an unconditional deoptimization exit at that
operation. A tagged value is computed on through `GRJIT_OP_BITCAST` (a `REF`
reinterpreted as an `I64`, shifted, added, tagged and reinterpreted back).

**The guard and the exit.** A guard that fails, or an unconditional exit, returns
from compiled code with a frame state in the guest frame's own slot order: the
function word, the pc, the sp, the flags (dead: the frame keeps its own), one slot
per local, one per operand-stack position and last the fuel the compiled code had
counted and not charged. The deoptimizer writes pc, sp, the locals and the stack
into the guest frame, adds the fuel to the execution's pending fuel, and the
interpreter resumes at the operation, which it re-executes whole: an operation that
deopts has not been charged, because its guard is tested before its cost is added.
A function that deoptimizes eight times is discarded and never compiled again in
that execution (`functions_discarded`); one whose first operation after its entry
poll is an unconditional exit is not compiled at all.

**Polls, and the frame at a GC point.** Compiled code never pushes or pops a
frame, and calls no allocating, polling or guest-calling helper. Its only calls
out are the two fuel helpers (no GC point) and the poll helper. A compiled poll
is one call of the fuel helper (every poll charges and flushes fuel, as the
interpreter's does), then a load of the request word and a branch; the poll helper
is called only when something is pending.
The slow path calls `gltang_jit_poll`, which finds the compiled frame through its
own frame-pointer chain (the JIT module is built with `-fno-omit-frame-pointer`, a
stamped Makefile flag), reads the frame state of the site with
`grcore_deopt_read`, writes pc, sp, the locals and the stack into the top guest
frame, polls with the same identity as the interpreter's own poll, reloads the
guest frame (the stack may have moved), and on continue copies the slots back with
`grcore_deopt_write_back`, so that a collector that updated a reference in place is
honoured. A pause or an unwind returns from compiled code with the verdict and the
guest frame already current: the interpreter returns paused or jumps to `unwound`
exactly as for its own poll. The poll helper is the one GC point, and at it the
guest frame is current, so a collection, a pause, the debugger and the frame
differential all see an ordinary interpreter frame, and no native frame is ever
scanned by the collector (AD-17).

**Fuel (AD-21)** is the same on every tier. Compiled code counts each executed
bytecode's cost in a register and flushes it to the execution before every poll
(`gltang_jit_flush`, which also flushes to the context, as the interpreter's `POLL`
does), on `RET`, and in the deoptimizer on exit. A budget therefore pauses or
unwinds at the same poll with the same total on both tiers, which the fuel-parity
test checks over four programs and three budgets.

**`LINE`** loads the execution's `statement_polls` byte at every execution, so a
host that turns the switch on mid-run (a test flips it from a poll handler) gets a
poll at the next compiled `LINE`, and with it clear a `LINE` costs a load and a
branch and no fuel. The engine never asks whether a debugger is attached: a
breakpoint in compiled code is a `LINE` poll whose vote the debugger casts, the
frame is written, and stepping resumes in the interpreter. A later call of the
function enters compiled code again.

**Ownership.** Compiled code is context-specialised in this story (it bakes in the
program-and-function word and the execution's own addresses) and owned by its
execution's cache; its pages come from the context's counting page provider, so
they are on the context's meter and a memory budget can refuse them. A compile
refused by memory, by a limit or by `protect`, marks the function never-compile,
is counted (`compile_failures`) and the run goes on in the interpreter. `a/code.h`
holds the count: the cache owns one reference and each entry takes one.

### Why this and not something else

- **The guest frame is pushed first and compiled code updates it, vs rebuilding
  the frame on a pause.** The shadow frame makes four problems disappear without a
  new core feature: a push can fail only where the interpreter already handles it,
  a pause finds only interpreter frames (AD-8), a collection at a poll sees every
  reference as an ordinary slot, and the frame differential compares an interpreter
  frame with an interpreter frame. Rebuilding on a pause would put a frame builder
  in the pause path and make the debugger's view a function of the JIT. The price
  is that the guest frame is stale between polls, which is harmless because
  compiled code has no GC point between them.
- **The frame is written at every poll's slow path, vs a precise native walk of
  compiled frames.** A precise walk would let compiled code call allocating
  helpers and hold references across them, which is what a wider supported set
  needs, and it needs the collector's root source to read native frames, which
  `runtime-core` does not have. Writing the frame is cheap where it happens (only
  when something is pending) and the walk is the next step.
- **Tier-up in ACT with entry after the entry poll, vs on-stack replacement.**
  Entering at a function's start needs the state at one place, with an empty stack.
  Replacing a running interpreter frame at a loop's back-edge needs a mapping from
  every loop head's interpreter state to compiled registers and an entry point at
  each. A loop that is hot in a function called once stays in the interpreter for
  that call: the first measurement shows what that costs and the next step is to
  say whether it matters (AD-26).
- **A counter on `POLL` vs call counts.** A poll is the interpreter's one common
  place for entry and back-edge, so one counter sees a hot loop that a call count
  would miss. The cost is one load, one increment and one compare per poll when
  tier-up is on.
- **Helper calls for fuel vs inline.** Two helper calls are two places where the
  count is added to an execution field, and the baseline keeps every register in a
  frame slot anyway. An inline add of the running count to `pending_fuel` would be
  a store the poll's fast path does not need, and the interpreter already charges
  where it charges. Every poll costs that one call whether or not anything is pending; the poll helper costs more and runs only when something is.
- **The small supported set vs a wider one.** Every operation compiled inline is
  one with no GC point, no allocation and nothing to restore on a failure but its
  operands. A wider set (calls, globals, containers) needs a precise native walk,
  the call protocol between compiled frames, and barrier code the engine passes in;
  each is a story.
- **The `LINE` flag read at every execution vs forcing the interpreter when
  statement polls are on.** Forcing the interpreter would make a debugger detach a
  function from the JIT, so a breakpoint test would test the interpreter; reading
  the flag costs a load and keeps the compiled function the one that stops.
- **Context-specialised code vs shared code.** Code that bakes in an execution's
  addresses is a smaller compiler and no indirection; sharing between contexts
  (AD-22) needs a context register for every address and a cache keyed by program,
  which `a/code.h`'s atomic count is ready for and nothing else is.

### Measured


First measurements (`make bench` on an Intel Core 7 150U, gcc 14.2 -O2, release, best of seven, calibration 1.12 ns per xorshift step; an engine case's unit is one run of a fixed program, setup excluded; nothing was tuned, AD-26). The loop is `function f(n) { i = 0; while (i < n) { i = i + 1; } return i; }`, called once with n = 10,000,000, with the JIT off and with a threshold of one, so that the compiled run includes the compile and the entry:

| Case | Figure |
| --- | --- |
| the 10-million-iteration integer loop, interpreted | 359 ms (36 ns an iteration: a poll, a comparison, a jump and one statement) |
| the same, compiled | 111 ms (11 ns an iteration): **3.2 times faster** (3.3 in the run before it) |
| the same work as four statements an iteration (2.5 million iterations), interpreted | 164 ms |
| the same, compiled | 63 ms: 2.6 times faster |
| a poll with nothing pending, from the two loops | the loop skeleton (a poll, a comparison and a jump) costs 26 ns interpreted and 6.4 ns compiled, and a statement `i = i + 1` 9.9 ns interpreted and 4.7 ns compiled. A compiled poll is a call of the fuel helper, a load of the request word and a branch; the interpreter's is the fuel flush, `SAVE`, `SYNC`, the call of `grcore_stack_poll` and a reload |
| a typical small function (a comparison, a branch, a 3-iteration loop), run once | 0.52 us interpreted; 76.4 us with a threshold of one, which is the compile and the entry of two functions (the top level and `f`), about 38 us each |
| what the JIT's presence costs a run that never tiers up (the same case, `JIT=yes` with the default threshold against `JIT=no`, alternating) | loops and string and array building: +2 to +5% (a counter on each poll of a function that is not the main program's top level, and the registration of the tier-up handler at creation); native calls, `use` and the error list within the noise |
| `fib(15)`, 1,973 calls | story 15 measured 180 us without the JIT and 220 us with it (+22%). Re-measured on the same tree (the `fib-15-*` and `fib-22-*` cases of `bench/`, best of a thousand and of a hundred runs): 201.8 us without the JIT and 236 us with it (+17%); `fib(22)`, 57,313 calls, 5.86 ms and 6.15 ms (+5%). Two costs, only one of them avoidable. **Per call:** every entry poll of a function the JIT had already given up on still called the counting function and the entry function, about 5 ns a call; a function that has settled (discarded or refused) is now remembered in `jit_settled_fword` and costs one compare. After that change: `fib(15)` 225 us (+12%), `fib(22)` 5.93 ms (+1.2%). **Once:** `fib` is compiled after 200 polls (about 25 us here), every call enters compiled code and leaves it at the `CALL`, and after eight such exits the code is discarded. Keeping a compiled caller's calls inside compiled code would remove that, and is what the story's guardrail forbids (no JIT frame calls a JIT frame: AD-8, AD-17, a collector never scans a native frame). A rule against compiling loop-free functions was considered and not taken: it would also stop compiling a straight-line arithmetic function called a million times, which the 200-poll threshold already prices correctly. A call-heavy function remains what a baseline without calls inside compiled code is worst at |

The gain is where the supported set is: a hot loop over small integers and booleans, called often enough to be entered, or entered once at a function's start. A loop in a function that is called once stays in the interpreter for that call (no on-stack replacement), and the main program's top level is never counted past its entry poll for the same reason.

### What is not done

- **Calls inside compiled code: a defect of milestone 1, not a choice.** A
  `CALL` is a deoptimization exit; the interpreter makes the call and the callee
  enters its own compiled code at its entry. JIT frames do not call JIT frames,
  so call-heavy code runs slower with the JIT on (`fib(15)` +12%, "Measured").
  Leaving calls out was a bug (Corey, 2026-10-05), and the spine's AD-9 now
  says a baseline without them is incomplete. It has its own spec,
  `planning/specs/spec-runtime-calls/`, and nothing here claims it is done.
  Floating point in compiled code is the same kind of defect:
  `planning/specs/spec-runtime-float/`.
- **On-stack replacement into a running loop.** Compiled code is entered at a
  function's entry only; a loop that gets hot in a function called once runs in
  the interpreter for that call, and after a pause at a compiled poll the rest of
  that invocation is the interpreter's too.
- **A compiler thread**, **code sharing between contexts** and **a cache**. The code is
  context-specialised and per execution, compiled synchronously in a poll; there
  is no cache on disk. (`a/code.h`'s count is atomic so that sharing can be added
  without changing who owns what.)
- **Other targets.** x86-64 and arm64 on Linux and x86-64 on Windows have a
  backend (the arm64 and Windows ones are runtime-jit's, with the same frame
  layout and the same saved-frame-pointer record at `[fp]` and `[fp + 8]`, so
  nothing here changed for them but this comment: the poll helper finds the
  compiled frame the same way under mingw GCC, which the JIT tests, the JIT arm
  of the frame differential and `jit_hot_loop` show under wine, and have not
  shown on a Windows machine). Windows arm64 and macOS have none:
  `grjit_backend_available()` is false there and the compile is then
  `GLTANG_ERR_UNSUPPORTED`, counted as a failure. `JIT=no` is the arm for every
  other target.
- **Inline caches**, **inlining of heap operations** (arrays, maps, strings, calls
  to natives), floating point, boxed integers and `DIV`/`MOD`: all of them are
  exits, and the interpreter does them.
- **Hardening** of the generated code (guard pages, randomised layout, constant
  blinding): the pages are never writable and executable at once, and that is all.
- **A precise native-frame walk for roots.** Not needed here, and what a wider
  supported set would need first.
- **Compile-time limits tuned to a benchmark.** The supported set, the threshold
  of 200 polls and the eight-deoptimization rule are the story's figures, taken as
  given; nothing was tuned (AD-26).

## Snapshots

A snapshot (story 16, CAP-11) freezes an execution that is paused, or has not
started, into an immutable object, and `gltang_snapshot_restore` makes a fresh
execution continue from it: the host creates a context, a heap and an execution
as it would for a fresh run, attaches the libraries, restores, and calls
`grcore_resume` (or `grcore_run` for a snapshot of a new execution). The point is
a start-up that is paid for once: a template that builds a table before it serves
anything is run to that point, frozen, and every later context starts from the
frozen state. The golden flow is in `examples/snapshot_start.c`.

### How it works

Three libraries carry it, each behind the seam the spine names (AD-19: hooks on a
key; AD-12: nothing is keyed by address).

- **runtime-core** keeps a snapshot as one named blob per key that has hooks, and
  restores atomically (CHECK, APPLY, PREPARE, COMMIT: see its `design.md`). The
  guest stack's own key writes the frames, with every `VALUE` slot written as zero.
- **runtime-heap** writes an image of the objects the context's roots reach, in
  the order the collector walks them, every reference an index, every type a name
  (its `design.md`, "Heap images"). It writes the *values* of the roots and of the
  frames' slots, which is why this library writes none of them.
- **lang-tang** (`src/vm/snapshot.c`) writes what only the engine knows, in the
  key `lang-tang execution`: the state (`PAUSED` or `NEW`); the main program's
  identity; the number of root slots; the pause location; the output buffer (its
  bytes, its committed length and its typed segments); the error list (every
  entry, its chain and its strings, and the count dropped); and the templates that
  have already run, each named. It also gives three types per-object hooks.

**What is captured, and what the host supplies again.** Captured: the guest
stack's frames, the heap's live objects with their stable IDs, the execution's
roots, temporaries and constants cache (as slots, in the image), the state and
pause location, the output so far, the error list. Supplied again, as for a fresh
run: the group, the options and budgets (fuel used starts at zero in the restored
context and the limits are the destination's), the page provider and allocator,
the port and requests, the program, the libraries, the seed sequence, the name,
the halt and logging switches, the statement polls and the JIT threshold. A
generator `random.global` that had been created continues from where it was (its
state is in the image); one that had not is made, when first used, from the
*destination's* sequence (a test makes the answer depend on the sequence to show
which one it was).

**The program is the destination's, and is checked.** The snapshot records the
function, constant and global counts of the main program and a hash of its
content (its file, code, line tables, names and constants: `gltang_program_identity`).
A destination built for another program, even one differing in one constant, is
refused before anything changes. The file name is in the hash: the error list
holds it, and a restored list must name the file the destination's own later
errors do.

**Host values are names.** A library object holds a pointer to a library; a native
function and a template hold one to a library member. None can be in a snapshot,
so each type has a `snapshot` and a `restore` hook on its runtime-heap type. A
library is named by *where it is*: the layer a `use` finds it in (the execution's
libraries, the main program's, the built-ins) and the names of the library members
that lead to it from that layer's root; a member by its library's path and its
own name. A path rather than a library name because the root of a layer is
usually unnamed (the host's own library of natives and templates), and because a
path is unique where a name need not be. The take checks that the path leads
back to the object and refuses if it does not; the restore resolves it in the
destination's layers, requires the member to be the kind it was (a native, a
template), and refuses a name that does not resolve. A template that has already
run left a program in the execution (and the constants it made): it is named the
same way and must have the same program identity. A name that does not resolve
refuses the restore with the destination exactly as it was, and a test runs that
destination's own program afterwards to show it.

**When a snapshot may be taken** (AD-20): by the owning thread, with the execution
paused, or new and parked outside `run`, with no template call in flight, no host
or native frame and no budget scope. The heap must hold no C root, handle, pin or
weak cell and no root source may report a conservative range (runtime-heap refuses,
and the take is `GLTANG_ERR_INVALID` with a named reason in that library's
design). Reading a running or at-poll execution is never allowed, a finished or
unwound one has nothing to resume and is refused too.

**Atomic.** A failure at any point leaves the destination a fresh, runnable
execution: the partly built state is released and the execution is `NEW`. The
engine's APPLY allocates everything into locals first and installs last; its
ABANDON puts the execution back to what `gltang_execution_create` made. The same
holds under allocation failure at every point of the restore (a test fails the
Nth allocation for every N and then runs the destination's own program), for the
destination's memory budget (`GLTANG_ERR_LIMIT`, every byte charged given back),
and for its depth budget.

**The tiers.** The JIT's code and feedback are not in a snapshot: a paused context
is always interpreter frames (AD-8), and the destination's own
`gltang_execution_create` attached its own JIT, which tiers up again from nothing.
Fuel is charged the same on every tier, so the fuel used before the pause plus
the fuel the destination uses equals what the uninterrupted run used, which the
sweep checks for every program at every pause point, in every pair of arms
(`JIT=yes` at threshold 1 into an interpreter-only destination and the reverse).

### Tests

`tests/unit/test_snapshot.cpp` pauses every executable program of the execution
corpus at up to five fuel points, takes a snapshot, restores it into a fresh
context (the first point of each program on another thread) and requires the
output (raw and rendered), the result, the error list and the fuel to be those of
the uninterrupted run. Then every row of the story's matrix: another thread,
restoring twice and releasing the snapshot early, a new execution, output and
errors so far, library, native and template values by name (and a missing name,
and a name with another kind), stable IDs and the next ID, every refusal (running,
at-poll, in a host call, a template call in flight, a C root, a handle, a pin, a
conservative range, a finished execution, another thread), every mismatch (a
program, an engine table, a set of keys, a heap codec, an execution that is not
new), allocation failure at every point of take and of restore, the destination's
memory, depth and fuel budgets, six threads restoring one snapshot, and the
collector's torture, barrier-verify and a moving guest stack over a restored heap.
A scan of every blob for every host address (the libraries and their members, the
programs, the context, the heap, the execution and every root object) shows there
is none. `make test-torture` and `make test-tsan` run the same suite under their
instruments, and `make test` runs it with the JIT at threshold one as well.

Two planted defects show the instruments are wired to the code (`tests/planted/`
11 and 12, "Planted defects in the library"): a type that forgot its hook (the
address scan finds the library member's address in the heap blob) and an output
buffer that is not captured (the output-so-far test and the sweep fail).

### Why this and not something else

- **A snapshot of memory.** Copying the heap's pages and the context's structures
  would need the same addresses at the destination, which the page provider does
  not promise and which two contexts in one process could never both have; it
  would also carry every function pointer and every host pointer into a "frozen"
  object (rejected in runtime-heap's and runtime-core's `design.md`, which say why
  at length).
- **Snapshotting an execution in the middle of a template call.** A template call
  is an activation with its own variables, output buffer and budget scope, and its
  own program on the stack. It could be captured, and the guest stack's refusal
  with an activation record is AD-20's rule, which this story is bound by: a
  snapshot is of a context that is idle or paused with no host frame above `run`.
  The templates that have *finished* before the pause are captured (their programs
  and constants), since a later call needs them.
- **A byte format or a file.** The snapshot is an in-memory object that holds no
  address, which is what makes a file possible later; making it one now would
  decide a format, a version and a loader that is a parser of untrusted bytes
  (AD-24) before anything needs it. Recorded as not done.
- **Sharing compiled code or feedback through a snapshot.** Compiled code is
  per-execution and context-specialised here (AD-22), so there is nothing to share
  yet, and feedback lives with the context; the destination's JIT starts cold and
  is correct either way.
- **Naming a host value by a single library name.** The root library of a layer
  has none and names are not unique across layers; the path is.
- **Restoring the output by replaying it.** The bytes and their encoding tags are
  copied; replaying the prints would run guest code.

### What is not done

- No serialisation to bytes or to a file, and no snapshot that survives the process.
- A snapshot is refused with a template call in flight, a finished execution, a
  C root, a handle, a pin, a weak cell or a conservative range. (This engine has
  no weak cell; a host that adds one to the heap cannot snapshot it.)
- A template that has run must be reachable from the execution's layers (the
  libraries, the main program's, the built-ins); one reachable only from another
  template program's own libraries is refused. A native or a library the program
  holds must be too.
- A restored context reports no pause *keys* (which key caused the pause), only
  the line; the first resume decides again, since fuel and requests are levels.
- No sharing between a snapshot and a group; a restore touches only the context it
  is given.

## Profiling and retention

Two questions a developer asks of a running program, answered by services of the
runtime stack and not by this engine: *where does the time go* (CAP-12, a
sampling profiler) and *why is this value still alive* (CAP-13, a retention
query). The engine's part is only what it already does: it polls, it pushes
frames with a poll identity and a locator, and its values are heap objects
described to the collector. Neither needed a change to `src/` (one comment).

**Profiling.** `runtime-core`'s `b/profile.h` is a keyed OBSERVE service. A host
attaches it to the engine's context (`grcore_profiler_attach`), starts its timer
(`grcore_profiler_timer_start`, a thread that posts a request) or posts requests
itself, and reads a report of `(file, line, self, inclusive)` after or during a
pause. The next poll after a request walks the frames through the abstract frame
and counts them, which is the engine's `locate`: `gltang_vm_location` gives each
frame's file and line, and the file string is the program's own (the profiler
keeps the pointer, so a profile that outlives the program copies the names; the
example does). Because a poll identity is the same on every tier (AD-18), the
interpreter's profile and the JIT's are the same, and `tests/unit/test_profile.cpp`
checks that: it asks for a sample at every poll, counts what each sample must hold
with a frame walk of its own, and compares, location by location, with the
profile, with the JIT off and with every function tiering up at its first poll,
for a program with calls, a recursion and a template call, and then compares the
two profiles with each other. Two loops run four to one are seen four to one. The
timer is tested statistically (the hot loop of a program with a short prologue
gets most of the samples), on both tiers, and extends the run until a floor of
samples was taken instead of asserting on a handful.

**What is and is not done.** The profile is biased to polls: a sample is taken at
the first poll after the request, so time between polls is charged to the poll
that ends it, and in a loop that is the loop's back-edge, which the report names
by the loop's own line (the `while`). That is the price of reading a guest stack
only where it is consistent (AD-4). There is **no `tang` flag** for it: a host
registers the profiler (`examples/profile_hot_loop.c` shows how), because the
command is a host of the debugger and of nothing else. There is no flame-graph
output and no call-tree: a profile is flat counts by location. A sample costs
about 100 ns at depth one and about 30 ns for each further frame, and at a
millisecond the timer's cost does not show beside a ten-million-iteration loop
(`profile-loop-10M-off` and `-1ms` in `bench/`, 353 and 352 ms).

**Retention.** `runtime-heap`'s `grheap_retention_path` finds the shortest chain
of references from a root to an object. `tests/unit/test_retention.cpp` pauses a
program by fuel with a string held by a local variable and by an array, reads the
string's address through the frame's scope interface (the way a debugger lists
locals), runs on until the program has cleared the variable, and asks: the answer
is the guest stack's root source (`runtime-core.guest`), then the array, its
storage and the string, with the byte offset of the slot that holds the next in
each. The same with every function tiering up at its first poll: a pause at a
compiled poll leaves a frame the interpreter can read, because the frame is
written back before the poll (AD-17), so the retention query reads the same
guest stack either way. An object nothing reaches is "not retained", and a
collection then frees it, which the test shows.

One thing the test found about the language: an array stored in an array is a
*copy*, so the inner array is not the object the outer one holds (the first
version of the test leaked an array into an array and the query rightly said the
original was not retained). The leaked object in the test is a string, which is
immutable and shared by reference.

**What is not done.** The chain stops at the guest stack's *root source*, with the
slot's ordinal in that source's enumeration, and does not say which Tang variable
the slot is (a host that wants the name walks the frame as `find_local` does). The
query cannot say how much an object *retains* (the set of everything it keeps
alive is a heap-dump tool with another design), and runs only while the context is
paused, parked or in a poll handler.

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
mode and again with a stack that moves on every push (the suites are
`testExecute_simple`, `testExecute_complex`, `testEngine`, `testCompile`,
`testLibrary`, `testRandom`, `testErrors` and `testTemplate`), the CLI test, the
fuzz replay (three harnesses), the oracle differential, and one smoke run of the
benchmark. `make test-torture` repeats those suites under ASan and UBSan and
`make test-tsan` runs every suite under ThreadSanitizer, including the ones that
hop a paused context from thread to thread - among them a context paused inside a
nested template's scope and resumed on another thread (`testTemplate`), which
gives the same output, error list and result as an uninterrupted run. `check-install` proves what `make install` leaves
behind is usable by a consumer that includes only the umbrella.

CI exists (`.github/workflows/ci.yml`, commit `91bc6fd`) and has never run on a
runner: GitHub Actions is disabled for cost, so no workflow fires on a push. A
local `make test` (plus `tools/xarch` and `tools/xwin` for other targets) is the
whole gate, and a claim about a result names where it ran.

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
| run: `use math; s += math.pi` 200 times | about 35 us (about 180 ns for a `use`, a member access and an add) |
| run: a native function called 1,000 times | about 59 us (about 59 ns a call, the loop included) |
| run: a template call, 200 times (three prints each) | about 107 us (about 535 ns a call: the activation, the scope's open and close, the prints and the output string) |
| run: 2,000 swallowed errors, the list full after 1,024 | about 208 us (about 100 ns an error; the 976 past the cap are only counted) |
| run: `random.global.next_int % 7` 1,000 times | about 123 us (about 120 ns an iteration, a boxed integer or two included) |
| run: four statements to a loop iteration, 1,000 iterations, statement polls off | about 65 us (the `LINE` instruction is executed and does nothing) |
| run: the same, statement polls on with nothing pending | about 130 us (about 16 ns a poll on the unarmed fast path) |
| start from scratch: a context made and run until it pauses right after a heavy prologue | about 2.80 ms |
| start from a snapshot: a context made and the snapshot of that pause restored into it | about 0.234 ms (see below) |

The host API adds nothing to a run that does not use it: the loop and `fib` cases
are where they were, within the noise of the machine. (With the JIT built, the
default threshold costs a run that never tiers up 2 to 5%, and `fib(15)` 12%, most of it the
compile; see "The
baseline JIT", which also has the cases that measure the compiled loop.)

**The statement-boundary instruction (story 13).** `LINE` is executed even with
statement polls off, so the cases above that run statements are slower than they
were before it existed: re-measured from the tree before it (83c1fac) and from
this one, alternating, the loop is 64.6 us before and 66.7 us after (+3%),
`fib(15)` 181 us and 192 us (+6%), the template call 107.5 us and 111.1 us (+3%)
and the polling loop 67.6 us and 69.8 us (+3%). The calibration case moved by
under 3% between the same runs, so the figures are good to about that. The
table's rows are the figures before the opcode, re-taken on the machine of this
measurement (the template call was about 101 us when first recorded). That is the price of making a statement a place a host may ask to stop at without
patching a shared program; the alternatives are listed under "Statement polls".

**Profiling (story 17, CAP-12).** `profile-loop-10M-off` and `profile-loop-10M-1ms` run the same interpreted ten-million-iteration loop without a profiler and with one whose timer posts every millisecond: 353 ms and 352 ms, the difference being inside the run-to-run noise. A sample is a frame walk (about 100 ns at depth one and 30 ns for each frame more, measured in `runtime-core`'s `profile-sample-*` cases), so a 1 kHz timer takes on the order of a ten-thousandth of the run.

**Snapshots (story 16, CAP-11).** The two `start-*` cases measure the same
thing two ways: the time from creating a context (group, context, heap and
execution) to its being paused at the ready point, where the ready point is
right after a prologue that builds a 20,000-element array and a 2,000-entry map
in loops (about 2.4 million units of fuel, which leave 7,127 live objects).
*From scratch* runs the prologue, under a fuel budget that stops it just after
it; *from a snapshot* restores a snapshot taken once at that pause (936,922
bytes, from `gltang_snapshot_size`), outside the clock. First measurement, on an
Intel Core 7 150U (12 threads), gcc 14.2.0, `-O2`, the JIT built, best of seven,
the calibration case beside them at 1.11 ns:

| Start | Best | Median |
| --- | --- | --- |
| from scratch | 2,804,352 ns | 2,810,547 ns |
| from a snapshot | 234,018 ns | 234,932 ns |
| saved | 2,570,334 ns (91.7% of the scratch start) | 12.0 times faster |

The saving grows with the prologue (a restore costs the heap's size, the prologue
its fuel) and shrinks to nothing for a prologue of a few statements, which a
snapshot is not for. A restore of this heap is about 0.2 ms beyond making the
context, which is about 30 ns for each of the 7,127 objects: a cell from the
heap's own allocator, a copy, the patching of its slots and (with the instruments
off) nothing else.

## Fuzzing

`tests/fuzz/fuzz_parse.c` and `fuzz_template.c` are libFuzzer harnesses on
`gltang_parse` and, for an accepted tree, `gltang_compile`; `fuzz_run.c` parses,
compiles and runs, under a small fuel, memory and depth budget, so a loop or a
runaway recursion ends as a pause or an unwind (neither is a failure) and the
result and output are read after (`make fuzz-parse`, `make fuzz-template`,
`make fuzz-run`; run with `make fuzz-run-parse` and so on; clang). The first
byte of a `fuzz_run` input picks script or template (bit 0), enters every error at
creation (bit 1) and halts on the first (bit 2). The harness registers a native
function, a `user` library, a string, and three templates with tiny scope budgets
under the three policies (a finishing one under EMPTY, a runaway one under PAUSE,
and one that calls the first and divides by zero under SEGMENTS), a seed sequence, and a cap of 64 on the error
list, so the fuzzer reaches resolution, native calls, scopes and the list; six
seed files under `tests/fuzz/corpus/run/` do, and are replayed in `make test`. `make fuzz-replay` feeds
every corpus and seed file once through the same entry points in an ordinary gcc
build and fails on a crash - it is part of `make test`, so a regression a fuzzer
once found is a failing test and not a campaign to repeat. Fifteen seconds of
`fuzz_run` (307,000 executions, 2,940 new units) found nothing on the first
run. The differential fuzz run, which generates valid programs and runs them on
both engines, is a different thing and is described under "Verification".

## What is not here

No library other than `math` and `random`, and in those no `next_int_range`,
`next_float_range`, `next_gaussian`, `shuffle`, `sample` or `choose`: they stay
`Not implemented`, as in ctang. No arrays or maps injected from the host (scalars,
native functions, templates and libraries only), no template arguments, no
`include`, no `try` or `catch` (no new syntax or semantics at all). The execution
differential against ctang is described under "Verification"; the ledger is
closed (no open row). No debugger in the library: the engine registers the frame
protocol (the frame walk, scopes and variables read from a paused context, which
name the right program for every frame) and polls at statements when a host asks
("Statement polls"); `runtime-debug` is the debugger and `tang --dap` and the
web-server example are its hosts. No conditional breakpoints, no expression
evaluation. No `simplify`. Snapshots are in-memory objects, taken of a paused or new
execution with no template call in flight: no byte format, no file and no
sharing of compiled code ("Snapshots", "What is not done"). The JIT is a baseline: it is described, with
what it does not do, under "The baseline JIT". No
parse-time charge to a context's memory (see "The memory-budget contract"). The CI
workflow has not run on a runner (see "Gates").

**Story 9's deferred items.** The resolver contract is closed: host callbacks
are opaque, run on the owner thread and cannot re-enter (see "The callback
contract"). Two stay open, with their reasons. The value renderer still holds a
512-byte buffer per recursive frame, so value depth 2,048 needs about 1 MiB of C
stack; it is bounded and a template call does not add to it (a template does not
recurse in C), but a host thread with a small stack is still its risk. The
compiler's operand-ceiling `GLTANG_ERR_LIMIT` arms (16 million constants) have no
test, since reaching them needs a test-only override of `GLTANG_OPERAND_MAX`.
