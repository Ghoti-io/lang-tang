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

The language's reference is [language-reference.md](language-reference.md). It
opens by saying that ctang's reference still describes everything it does not
yet cover, and that where the two differ it wins and the divergence ledger lists
the difference. It carries what lang-tang has decided so far, which is how
numbers print (every NaN as `nan`, D-031).

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
| `src/unicodeString.c`, `unicodeString.h` | the same names | The string type, `create`, `create_and_adopt`, `destroy`, and (story 4) `concat`, `substring`, `render`, `html_encode` |
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
  Concatenation, substring, rendering and HTML encoding were first internal
  ("they come with the interpreter"); story 4 of `spec-tang-host-extensibility`
  made them public again (the ledger's D-056). The four escape scanners
  (`htmlEscape`, `htmlEscapeAscii`, `percentEncode`, `unescape`) are not public:
  the engine has its own encoder (`src/vm/string.c`). A stray file-scope
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
to about 130 us, about 4,000 statements. (These are that story's figures, on its own
machine and compiler; the table under "Benchmarks" has the cases re-taken on 2026-10-09,
where the poll is about 12 ns a statement.) No budget is asserted (AD-26).

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
`GLTANG_WORK_BYTES_PER_FUEL` bytes it returns. The call object points at a copy
of the arguments in the frame of the shared wrapper that enters the native (next
section), made before anything can collect and pinned by the wrapper's record,
which is sound because the function cannot reach a GC point or push a frame. A
string answer is copied when it is set, since the function's buffer may be gone
when it returns. Returning false with nothing set is the error `Host function
failed`; returning with nothing set is null.

**Rejected: letting a native call guest functions.** It would make the native a
nested activation that cannot pause, and a pause inside it would have to be
refused by the poll (AD-5), which is what AD-23 states instead. That stays true of
the library API a host uses; the engine itself has the two shapes AD-23 names for
its own natives (a nested activation, and a resumable native), reached only by
the test natives below.

### Entering a native: one wrapper for both tiers

`gltang_vm_call_native` (`src/vm/libvalue.c`) is the only place a native is
entered, from the interpreter's `CALL` and from the thunks compiled code calls
(`src/jit/natives.c`) alike (spec-runtime-calls story 9, AD-28, AD-23). It copies
the callee and the arguments into an array of its own frame before anything can
collect; opens a `GRCORE_ACTIVATION_NATIVE` record whose conservative segment is
exactly that array (cleared first, so that every word the collector reads is defined,
which Valgrind asks and a fake stack of a sanitizer does not break), so the objects
the copies name are pinned and the native reads the copy and not an operand stack a
collection may have moved; runs the native; and leaves the record. The segment is not
the whole frame, nor the stack from the native to the compiled caller as the runtime-jit
fixture's `NativeScope` gives: the arguments are the only raw references a
call from compiled code hands over (they are not in the site's stack map), and an
earlier version that gave the frame and the stack above it faulted under a sanitizer's
fake stack and read uninitialised words under Valgrind. The record enters the native-depth budget (AD-21), so a
program that nests natives is limited by `grcore_options_set_native_depth`; a refused
record is the call's value, the recursion-limit error for the depth budget (as the
guest-depth budget's refusal is) and the out-of-memory value for the memory budget (after
the budget's verdict poll). Entering a nested activation is a GC point (AD-17): the heap's
torture mode owns only the GC points the heap has, so under torture the wrapper collects
once its record is open (the test switch `native_gc_seam`).

*Depth is the same in both tiers.* An interpreted run opens no record, so a nesting of
natives costs it one unit of the native-depth budget a level; under compiled code each
compiled run has a JIT record that costs a unit too, and a compiled nesting would be
refused sooner. A native called from compiled code therefore hands back the units the
open JIT records cost for as long as it runs (`gltang_jit_lend_depth`, taken back when it
returns), so the budget the native, and any re-entry it opens, sees is the interpreter's.
A compiled entry that the budget refuses is "not entered" and the interpreter runs the
function, which the output cannot tell. The same budget therefore refuses the same call,
at the same poll and fuel total, whichever tier runs it (the sweep in `test_native_calls.cpp`
nests re-entering natives under every budget from 1 to 24 and compares fuel, polls and error
list). Memory timing is the one thing that may differ: the array of records grows by doubling,
and a compiled run holds more records.

*The test natives* (`src/vm/testnatives.c`, registered by a test through the internal
`gltang_vm_test_add_native`; no header names it) are the cases an opaque host function never
meets: `depth` and `records` (what the wrapper opened), `echo`, `alloc_ref` and `alloc_n`
(natives that allocate), `reenter` (calls a guest function as a nested activation,
`src/vm/nested.c`: a `REENTRY` record, the function's frame pushed, and the same interpreter
loop run on it until that frame returns, entering compiled code as it does for any function; a
pause inside it is an unwind that stops at the record), `deopt` (returns its argument and asks
compiled code to leave after the call, the status the library names `GRJIT_NATIVE_DEOPT`) and
`resumable` (the native calls `f` three times and returns the sum; it asks the interpreter,
which makes each call from its own loop and leaves a native bound to the native's state in the
caller's frame as the continuation, so a pause inside `f` resumes like any other, and the
continuation is called again with `f`'s value). A resumable native is reached only through an exit:
the JIT does not compile a call of a member that is flagged resumable.

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

### Host-described values

A host that wants a template to see `menu` as an array of maps of strings cannot
build that in the engine's heap: the heap belongs to one execution, moves its
objects, and is collected while the program runs. ctang offered
`gta_computed_value_array_create` and `map_create` for it, the rewrite dropped
them without a decision (ledger D-048), and `spec-tang-host-extensibility`
story 2 restores them as **descriptions** (`description.h`, `stable`).

A `GLTANG_ValueDescription` is a tree of nodes in the host's own memory (null,
boolean, integer, float, tagged string, array, map): plain data, no pointer into
any heap, buildable before any execution exists. The engine makes a **fresh
copy** of it in the heap of the execution that asks, each time a program
reaches it: there is no per-execution cache, no hoisting and no sharing between
two reaches, so a program that changes what it was given changes only its own
copy (planted defect 95). It is handed over in three places: a library member
(`gltang_library_add_value`), a lazy factory (`gltang_library_add_value_factory`,
called only when a `use` reaches it, never at registration, with an empty
description to fill), and a native function's answer
(`gltang_call_return_value`, a description built during the call by
`gltang_call_description` or a sealed one the host keeps and answers again and
again). The existing scalar `GLTANG_FactoryFn`, `GLTANG_HostValue` and
`gltang_call_return_*` are unchanged.

**The names.** The spine and the decision document call the object a
*description* and a handle to a node in it a *ref*, so the types are
`GLTANG_ValueDescription` and `GLTANG_ValueRef` and the functions
`gltang_value_*` (decided with Corey, 2026-10-16). `GLTANG_HostValue` was taken
by the scalar a factory fills in.

**What a node is.** A `GLTANG_ValueRef` is an index and the number of the description that made it (`owner`), so a ref handed to another description, or a zero ref, is refused with `GLTANG_ERR_INVALID` instead of naming some other node that happens to have that index (planted defect 103). A node has at most one parent (`GLTANG_ERR_INVALID` for a
second), and attaching a container under itself or under one of its own
descendants is refused too: the builder walks up from the container, which is as
long as the depth limit allows and no longer, so the check is bounded and a cycle
cannot be built (planted defects 94, 96, 97). A map's entries keep the order they
were set in, and a key set twice keeps its first position and takes the last
value, as `m[k] = v` does; the second value's node stays in the description and
counts against its limits, and the engine's own map fill collapses the pair when
it makes the copy. Keys are matched by their bytes only, whatever their tag.

**Strings.** A string or a key is checked once, when it is added, to be valid
UTF-8 and to carry a tag in range, and is refused otherwise (planted defect
100). Nothing checks it again: the materializer uses a trusted entry
(`gltang_vm_string_from_trusted_utf8`) that skips the validation and the poll
that `gltang_vm_string_from_utf8` makes, so a text is never scanned twice and
a concatenation is never scanned at all. The tag survives inside a container, in
`print`, in `as string` and through concatenation (ledger D-013's property,
tested by a differential against the same cells put into containers by the
script through library strings), and a map key's tag survives as well (planted
defects 86 and 87).

**Limits.** Three, each with a default and a hard ceiling:

| Limit | Default | Ceiling | What counts |
| --- | --- | --- | --- |
| nodes | 100,000 | 4,194,304 | every node, a duplicate key's included |
| bytes | 16 MiB | 256 MiB | the bytes of every string and key, and 16 for each node |
| depth | 64 | 2,048 (`GLTANG_MAX_VALUE_DEPTH`) | a scalar is 0, an array of scalars 1, an array of arrays of scalars 2 |

A host raises a limit with the size-prefixed `GLTANG_ValueLimits`; one past its
ceiling is `GLTANG_ERR_LIMIT` at creation. An addition that would pass a limit is
refused with `GLTANG_ERR_LIMIT` before anything is written (every check comes
first, then the two steps that can fail for want of memory, then the writes that
cannot), so a refusal changes nothing. The depth is kept as the *extent* of each
subtree (a scalar 0, a container one more than its deepest child), which makes
attaching a subtree under a deep parent a single comparison of the parent's level
and the child's extent, and growth below an ancestor a walk up that stops where
the ancestor already reaches as deep. The builder tests the limits on its side
(planted defects 91, 92, 93), and the materializer tests them again on its own:
the node and byte counts against the smaller of the description's own limit and
the engine's ceiling, and the depth by the recursion's own bound, whatever the
description says (planted defects 90, 98, 99, 101, 102). A description past a
limit can only exist through a test hook (`gltang_vm_test_description_limits`),
which is how the materializer's checks are reached. A text whose length would
wrap the byte count is refused before it is read.

**The materializer** (`src/vm/materialize.c`) is `copy_rec` with a different
source and the same rules. A container is made at its exact size and held in a
temporary root while its children are made; each pointer store goes through the
array and map calls, which use the collector's barrier; a map entry's child is
held while its key is made, and the two are stored by a call that allocates
nothing. Nothing collects between a child's return and its store (planted
defects 84 and 85: the container held by a C variable, and a float held across
the key's allocation as its raw bits). It returns `Out of memory`,
`Recursion Limit Exceeded` or `GLTANG_V_UNWIND` as values, and the run goes on
after the first two, as it does after any failed allocation.

**Charging.** The heap bytes of a copy go to the context's memory budget, as any
allocation does. Its work goes to the fuel: each node is charged, before it is
made, 8 bytes and the bytes of its string and key, through a pacer that polls
under the native `DESCRIPTION_MATERIALIZE` every `GLTANG_POLL_BYTES`, and what
never reached a poll is added to the fuel the run has pending (a copy of any size
runs the budget out and unwinds like a long string copy: planted defect 88). A
description built *during a call* is allocated from the context's own allocator,
not the process's (planted defect 89), so a native that builds an enormous answer
is refused by the memory budget while it builds, and its description belongs to
the call: it cannot be retained, and it is freed when the call returns whether it
was answered or not (the `callk` path included). The native gate has two rows for
`DESCRIPTION_MATERIALIZE`, one a reach in a straight line and one a reach in an
endless loop of a function that is compiled when every function is.

**Where the engine reads it.** `gltang_vm_member_value` makes the value of the
two new member kinds (`GLTANG_MEMBER_VALUE`, `GLTANG_MEMBER_VALUE_FACTORY`) and
`call_result` the value of an answered description. Every caller of the first
(`gltang_vm_resolve`, `gltang_vm_library_attr`, the interpreter's `USE` and
`ATTR`, and the JIT's member-load thunks) stores what it returns before the next
GC point, and already treated it as allocating, since a library string is made
there; `call_result` runs while the native's activation record is still open and
its arguments pinned. The JIT does not compile a reach of a described member:
`loadable` and `static_member` accept natives and libraries only, so a function
that reaches one leaves compiled code at the `use` and the interpreter makes the
value (tested with every function compiled at its first poll, for a member by
name, by a path through a library and for a factory). A path through a described
value (`use cfg.a.b`) materializes the whole member and reads the attributes of
the copy, as a library member is read; the benchmark measures a path read so that
a cost shows if it is one.

**Measured** (`notes/lang-tang/perf-history.md`, "Story 2: host-described values, measured"). The 20-link list that took 95 us through 20 engines takes 17.8 us as one host array, building the description in the clock, and 15.5 us when it is built once. A description of 50,000 integers costs 0.47 ms to reach (a script-built array of the same size, 0.42 ms); a string element about 36 ns, an integer about 9 ns. A path read through a described value copies the whole member (10 us for a map holding 1,000 elements). On the existing cases the stop rule found one repair (the call object's cleanup cost each native call 91 instructions, +1.1%; out of line it is +0.64%) and five float-print micro-cases that are 12 to 15 ns slower at identical instruction counts, which is code placement and was not chased.

**Snapshots.** A copy is plain heap data (arrays, maps, strings, numbers), so a
snapshot of an execution that holds one needs nothing of the description, and
`snapshot.c` names only natives, templates and libraries.

**What is not in a description yet.** The node kinds of a bound native, a
library and an instance of a host type (stories 3 and 5 of the spec). The node
kind is an open enumeration (`GLTANG_NodeKind`), and the host-facing constructors
for the new kinds are added with the stories that make them.

### Handles: natives and factories that touch the program's values

A native used to read an argument as a scalar or as flattened text and to answer
with a scalar; ctang's callbacks received and returned any value (ledger D-049 to
D-052). `spec-tang-host-extensibility` story 3 restores that with **handles**
(`handle.h`, `stable`; AD-13).

**A handle is a place in the execution's temporary roots.** `exec->temps` is
the stack of values the collector already reads as precise slots and rewrites
when it moves an object, which an operation that builds more than one object
uses to hold its parts. A callback's **scope** is a window onto it: the first
handle call of a callback opens the window (it records where it starts and takes
the next serial) and every handle is the serial and the position in the window.
A handle therefore survives a collection and a relocation with no further work
(planted defect 104 shows what happens when a table of values the collector does
not see is used instead). Closing the scope releases the window; the callback's
answer is read from its handle *before* that, in `call_result`, and nothing
allocates between the read and the caller's store (planted defect 114).

**The check.** A handle is `{serial, index}`. The serial is the scope's and is
issued by a counter of the execution's that never goes back (116), and every
call compares it with the frames the callback has open, so a handle of a
callback that has returned, or of another, is refused and never read (105). A
scope holds at most `scope_limit` handles (default 1,024; a context option,
`gltang_execution_set_handle_limits`), counted over all its frames; a handle that
cannot be made, for the limit or for memory, sets a flag on the call and the
callback's value is `Out of memory`, whatever it answered, with the run going on
(106). A callback that makes no handle opens no scope and pays for none of this:
the scalar native-call cases are level (the measurements below).

**Frames.** A callback that builds a table of 20,000 rows cannot hold a handle
for every piece of every row. `gltang_handle_push_frame` and `_pop_frame` open
and close a frame inside the scope: the handles made in it end with it, the
handles of the outer frames stay valid inside it, so a value made outside is
filled from inside (113). It is what JNI's local frames are for.

**Reading keeps the tags.** `gltang_handle_string_segment` gives each tagged run
of a string, its byte range and its `GLTANG_String_Type`, computed from the
segment words and the grapheme offsets; the copy a host makes can therefore
keep the encodings (107). `gltang_handle_text` gives a pointer into the heap
object: it is valid until the next call that can collect, and `copy_text` copies
out for anything longer.

**The labels, and the gate that checks them.** Every function in `handle.h` is
labelled `@may_gc` or `@no_gc` in its documentation (Doxygen aliases make the
labels print). `tests/unit/test_host_callbacks.cpp` parses the header, requires
exactly one label on each function and a row in its table for each, and calls
every function inside a native, under torture (a collection at every
allocation), with the heap's collection counter read before and after: a
`@no_gc` function must leave it alone (110), and an allocating `@may_gc` one must
move it, which shows the counter measures something.

**Constructors** make any value in the heap. A container pushed into a container
is **copied**, as the language copies it for `a[i] = b` (115), so the program
never sees two places sharing one object; a host builds from the outside in when
the containers are large. A string is checked as UTF-8 by the engine's own
constructor (invalid text is `GLTANG_ERR_INVALID`). `gltang_handle_make_library`
seals the library and the execution **retains** it until it is destroyed (111);
`gltang_handle_make_native(call, fn, user, bound, &h)` allocates a record the
execution owns (112) and the native reads its bound value with
`gltang_call_bound`, which is how `acct.name()` knows its account.

**Factories** (`gltang_library_add_handle_factory`) are given the same call
object with no arguments and answer by handle, so a factory can answer a
container, a native or a library, and `use pages.recent as r` works for a
library a factory made. Returning false is "not available" and binds null, as for
the scalar factory. The JIT does not compile a reach of the new member kind (as
for a described value), and a native is called as before.

**Persistent handles** (`gltang_handle_promote`) are a table of the execution's,
reported to the collector as roots (104, and 117 for a copy shown instead of the
slot, which only a heap that moves can see). A slot's generation is odd while it
is live and even while it is free, so a released handle is a tombstone and the
slot can be reused; the count is limited (default 4,096; 109); the table, the
libraries and the native records are freed with the execution (108, 111, 112). A
snapshot is refused while one is live, or while the execution owns a run-time
library or native (an image cannot name them; story 8 gives host values hooks and
the refusal its name).

**The state is lazy.** Everything the handle API keeps for an execution (the frames, the serial counter, the limits, the persistent table, the libraries and native records it owns) is one `GLTANG_HandleState`, made when a callback first asks for a handle; the execution holds one pointer. The first version put the fields in the execution itself, which grew it past 1,032 bytes, the largest block glibc's per-thread cache serves, and cost every engine of a request 400 instructions to make and free (`workaround-list-20-precompiled` +1.3%); the pointer keeps the execution at 1,000 bytes.

**The call object** gained `scope`, the index of its first frame, and two flags
(a handle could not be made; a constructor's poll ordered the run to stop). All of
them are zero for a call that never asks for a handle, and the cleanup of a call
is still two loads and a branch for one that sets no answer by description or
handle.

**Measured** (`notes/lang-tang/perf-history.md`, "Story 3: handle scopes, measured"). A native that never asks for a handle costs +0.72% instructions over the story-0 engine (`run-native-call-1000`; +0.9% in the A/B wall clock, inside its spread). A call that reads its bound value through a handle costs 1.17 to 1.22 times a scalar call (about 1,260 more instructions: a frame, two handle operations, a close). Building 20 lists of 20 rows through the handle API costs 6.8 us a list, 2.6 times less than handing the same list over as a description, which is the tool for plain data the host already holds. One existing case, `workaround-list-20-precompiled`, is +1.10% in instructions on a path with no handle code (allocator slow-path calls; not attributed, recorded); the wall-clock flags against the story-0 table are session drift and the A/B shows none.

### The host context, the language layer, the output sink and the string helpers

Four small pieces of the host API (`planning/tang-host-extensibility.md` 5.12;
ledger D-053 to D-056).

**The host context** is one pointer on the execution
(`gltang_execution_set_host_context`) that the engine never reads, copies or
frees. A native and a handle factory reach it through the call object
(`gltang_call_host_context`); a value factory through the description it is
handed (the in-run description copies the pointer; a description the host made
itself answers NULL). The scalar `GLTANG_FactoryFn` has a fixed signature in a
`free` header and cannot reach it: it is superseded by the handle factory, which
can answer a scalar. The setter is refused as the others are (a started run,
inside a host function). A snapshot does not carry it: it is the host's pointer to
the host's request, and applying a snapshot to an execution leaves that
execution's.

**The language layer** (`language.h`, `stable`) is one sealed library held by the
process, searched after the program's libraries and before the built-ins. It
is a process-level slot because ctang's was (`GTA_Language.library`) and because
a host registers the same functions for every program; the cost is one
process-wide piece of mutable state, so it has a lock: a spin flag held for a
load and a retain, or a swap and a release, and for nothing else. An execution
takes its reference when the creation succeeds and keeps it for its life (it is
the fourth layer of a `use`, named as layer 3 in a snapshot so that the index of
the three that existed does not move). Replacing the layer changes nothing for
a running execution, and a library is freed when the last execution that took it
is destroyed, not when the host lets go.

**The output sink** (`gltang_execution_set_output_sink`) delivers at the point
the engine already decides a print is whole. A print writes into the buffer and
sets `committed` when it finishes, and rolls back to `committed` when it fails,
so nothing before `committed` can be taken back. The sink is given everything
from the last delivered point to the end of the output, one call per tagged run,
before `committed` moves; if it refuses (returns false) the print is rolled back
and its value is `GLTANG_ERROR_HOST_FAILED`. Segments of a refused print that the
sink had already been given stay given: delivery is never retracted, and the
header says so. Only the main activation's buffer is delivered: a template call
prints into a buffer of its own that becomes the value the caller receives, and
that value reaches the sink when it is printed. The buffer is unchanged (the
sink is "as well as"); a host that streams a very large page and does not want
the buffer would need a mode that drops delivered bytes, which touches the
snapshot, `output_raw` and the committed offsets, and was not built. Applying a
snapshot moves the delivered point to the restored `committed`: what was printed
before is the snapshot's, not this sink's. The sink runs as a host function does
(the flag is set), so it cannot destroy or reconfigure the execution.

**The string helpers** are `concat`, `substring`, `render` and `html_encode` on
`GLTANG_Unicode_String` (`unicodeString.h`, `free`). `render` and `html_encode`
use the engine's own encoder, so the bytes are the ones `output_render` gives.
`concat` finds graphemes again across the join, as the language's `+` does, and
puts each grapheme in the run it began in, so it does not copy the grapheme
offsets of its inputs (ctang's did, and a combining mark at the start of the
second string stayed a grapheme of its own).

**Measured** (`notes/lang-tang/perf-history.md`, "Story 4 ... measured"). A native that reads the host context costs what one that returns an integer costs (0.96 times the scalar native call). A `use` that reaches a library of the language layer costs what a `use` of the execution's own library does. An output sink adds about 5% of the wall clock to a run that does little but print (+8% instructions: an indirect call and a constant-time search from the last segment for each print). On existing paths the cost is a layer's NULL test per `use` and about 105 instructions for each execution created and destroyed (the layer's lock, retain and release): `run-use-member-200` is +0.66% and `workaround-list-20-precompiled`, which makes 20 executions for one list, is +1.38%, over the 1% rule and recorded; a program that makes one execution for a request pays it once.

### Host-defined value types: the custom kind

A host gives a template its own values (an `account`, a `money`) through one new
kind, `custom` (`planning/tang-host-extensibility.md` 5.1 to 5.3; spine AD-29;
ledger D-032 to D-038). The public API is `type.h` (`stable`), which also declares the two execution
calls this kind added (`gltang_execution_attach_types`,
`gltang_snapshot_refused_type`) so that none of it is in the `free`
`execution.h`, and the custom-value calls of `handle.h`.

**One kind, one heap type.** An instance is one object of the engine's heap type
`lang-tang custom`: its kind, the number of slots, a pointer to its type, the
slots, then the raw bytes, zero-filled when it is made. The trace reads the slot
count from the object's own header, not from the type, so the collector never
follows a host pointer to trace an object (and an execution destroyed while its
heap still holds instances leaves nothing the trace needs). No module writes a
trace function (AD-11). Every slot store is `grheap_store_word` (the handle API's
`gltang_handle_custom_slot_set`, which copies a container first, as every store of
one does); raw bytes are copied in and out by offset and length and are never
traced. An instance is passed by reference, as a function is: `deep_copy` returns
it as it is, so `[m, m]` holds one object twice.

**The descriptor and the registry.** `GLTANG_TypeDescriptor` is size-prefixed: a
registry reads the members its own header knows, as far as the host's `size`
covers them, and copies them, so the host's struct and name need not live. A
registry is created by the host in the process allocator, holds each type in a
block of its own (a type's address never changes while the registry lives), and
is **sealed** by the first `gltang_execution_attach_types`, after which `add`
refuses and nothing in it is written but its reference count and, once, its
binding. A registry that holds a type that is not `thread_safe` is **bound** to
the first context that attaches it. The context is named by a serial the
execution takes at its first attach (a context holds one execution, so the
serial names the context), because an address does not name one: the tests ran
three contexts in a row at the same address before the binding used the serial.
Two registries that declare one name and version with two table hashes cannot be
attached to one execution.

**Identity** is (registry id, name, version), the id a process-wide counter that
never goes back. `gltang_type_same` compares the tuple, never the address; the
table hash covers the name, version, counts, `thread_safe` and the set of
operations implemented, never an address or a function. The registry
generation of AD-29's tuple is not built: a registry is immutable once sealed,
so it has one generation, and the generation joins the tuple when a registry
can be replaced (module identity, stories 11 and 12).

**The dispatch.** Every operation that switches on kind falls through, for a
custom operand, to `custom.c`. The test for a custom operand is placed only on
the paths that end in `Not supported` or reach the string rule: a pair of
built-in kinds pays for it only where it already fails, so `testBuiltin_pairs`
(the table of every pair of 21 operands through 12 operations and 10 unary ones,
with the heap objects each makes, written against `fb2b83e`) is unchanged.
Equality is the exception: a type's `equal` is asked before the weak-kind rule,
so `==` and `!=` test for a custom operand first. Two details keep the built-in code as it compiled before, which the first
measurement showed it did not: `gltang_vm_kind` maps an object's kind through a
table rather than a switch (the switch with one more case was no longer inlined
into `gltang_vm_truthy`, and every truth test paid a call), and the switches on a
value's kind keep `custom` in their `default` arm (an arm for the eleventh kind
changed the dispatch of `op_set_index` and its neighbours). The execution struct
keeps its size: the one new native counter took a padding hole's place.

**Reflection.** ctang's rule as written: the left operand's operation with
`self_is_lhs` true; if it declines, or the left is not custom, the same
operation of the right operand with the operands swapped and `self_is_lhs`
false; if that declines too, `Not supported`, made then. Before any of it, an error
operand of an operator other than `==` and `!=` is the value and no type is
asked, as a string's `+` gives its error operand: `money + (1 / 0)` is `Divide
by zero`, not money's decline turned into `Not supported` (decided by Corey at
the story's done checkpoint, 2026-10-20). Equality keeps its rule and asks the
type of any pair. Every built-in rule
declines a custom operand in this version (a string's `+` would need the type's
to-string, story 6), so `"x" + money` is `money`'s reflected add, never an empty
string. `!=` that both sides decline asks `equal` the same way and answers its
opposite (ctang's types implemented `not_equal` as the negation of `equal`; a
type that implements only `equal` would otherwise have a `!=` that disagrees
with it), and equality nobody answers is identity: two custom values are equal
only if they are one object, and a custom value equals nothing of another kind.
Inside a container comparison an element pair with a custom side asks `equal`
the same way, and only `true` makes the elements equal; an error it answers is
the whole comparison's value, as at the top level (an error is falsy, so a test
of the comparison still takes the not-equal arm; decided at the same checkpoint).

**A decline is a status.** The bracket returns a flag beside a null value; no
error object exists until every side has declined, so a decline allocates
nothing, never reaches the error-created hook (it cannot be logged or halt the
run), and cannot be mistaken for an operand's error. A test counts the heap's
objects and the context allocator's calls across a decline that the other side
answers: zero and zero.

**The bracket** is `gltang_vm_call_native`'s: the operands are copied into the C
frame the native activation record gives the collector as a conservative
segment (pinned for the call), the record takes a unit of native depth (a full
budget refuses it with `Recursion Limit Exceeded`, as for a native), the call
object makes a handle for each operand, `in_host` is set around the callback,
and the answer is read while the scope is open (`gltang_vm_call_settle`). The
callback is charged one unit of fuel, as a native call is, and one per 64 bytes
of a string it answers (on top of the string's own making, as for a native's
string answer). A callback polls with `gltang_call_poll`, which allows continue
and unwind only (the poll is the native `HOST_CALLBACK`). The operands are held
on the temporaries across both calls of a reflected operator and read again
after each. A setter with no answer of its own is worth the value assigned.

**`is_assignment`.** `a += b` is parsed as `a = a + b`; the parser marks the
binary node, and the compiler gives that operator instruction an operand of 1.
The custom dispatch reads it from the instruction being executed (the
interpreter records the instruction's place before it calls any operation), so
no built-in operation carries the flag (an argument would have been computed
and passed on every interpreted operation that leaves the small-integer path,
for the sake of the custom one). Nothing but a type's
operation reads it, and the JIT never looks at the operand of an arithmetic
instruction. ctang passed
false in every call (its parser desugars the same way), so this is new.

**What is not here** (story 6 and later): a custom value is truthy, `print`
appends nothing for it, an element of a container shows `Custom: <name>`, `as`
and string conversion are `Not supported`, calling one is `Invalid function
call`, and `for` over one is `Not supported`. A snapshot of an execution whose
heap holds a live instance is refused: the heap type's snapshot hook refuses the
image and records the type, and `gltang_snapshot_take` answers
`GLTANG_ERR_UNSUPPORTED` with `gltang_snapshot_refused_type` naming it. Compiled
code exits to the interpreter on a custom operand, as on any operand that fails
its guard (story 10 compiles the dispatch). Finalizers, weak slots, pins and
in-place byte access are story 7; module identity is stories 11 and 12.

**Measured** (`notes/lang-tang/perf-history.md`, "Story 5 ... measured" and "... at the reviewed commit"). On the EVO-X2 an add that a type answers costs 118 ns where the interpreter's integer add costs 29 (4.1 times: a native activation record, a handle scope with two handles, the callback and a new instance, and the bracket is the larger part); an attribute read that a type answers costs 84 ns where a map's costs 48 (1.75 times); a constructor that makes an instance costs a native call and a 24-byte allocation (93 ns against 60). Paths that existed before are level: every existing case is within 1% of the commit before this story in instructions (the largest rise +0.21%) and the ten-run wall-clock A/B against it puts none past its spread + 2%. The first cut was not level (three cases over 1%: `gltang_vm_kind` no longer inlined, a kind switch's dispatch changed by an eleventh arm, and the execution struct past a malloc size class), which is why `custom` sits in the `default` arm of those switches and the struct keeps its size.

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
runtime-core guest-frame function ID is a separate 32-bit field: Tang sets it
to the low 32 bits of the function word at every interpreter and native re-entry
frame push. Compiled code carries the complete program-qualified word as its
call token and declares the low 32 bits as its guest function ID; rebuild pairs
compiled and guest frames through that ID. A poll updates the opaque function
word and offset, but does not substitute for setting the guest function ID.
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

`seeds.h`, `description.h`, `encoding.h`, `valuekind.h` and `handle.h` are the host headers that are
`stable`: they include only stable headers (`core.h`, `allocator.h`) and are the C
embedding API's host entry for the seed and for a host-built value. `encoding.h`
holds `GLTANG_String_Type`, which `unicodeString.h` (free) used to hold, so that
`description.h` can name the segment tag without including a free header; the
enum is the same and `unicodeString.h` includes the new header, so nothing is
renamed. Two more fixtures show that `description.h` and `encoding.h` labelled free
are refused (`tools/check-gates.sh`). `valuekind.h` holds `GLTANG_ValueKind` for the same reason `encoding.h` holds the string tag: `handle.h` names the kind of a value, and `value.h` (free) includes the new header. `handle.h` also declares `GLTANG_Library`, `GLTANG_NativeCall` and `GLTANG_NativeFn`, which `library.h` used to declare, so the call object is an opaque stable type. `library.h` is
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
D-015, D-017, D-023 to D-030). The ledger is closed when no row is `open`. It was
at the end of milestone 1, and is not since the capability-removed rows were
added (D-032 to D-056; D-032 to D-038 and D-048 to D-056 are fixed, D-039 to
D-047 open): see "The ledger, final" in "Verification".

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

**Floats (spec-runtime-float, story 1).** The generator makes floats on purpose:
NaN of either sign and a payload (read from text, `"-nan(0x1234)" as float`),
the infinities, negative zero, subnormals, the least and largest normal, and one
ulp either side of 1, alone and as operands of each other (`inf - inf`, a
subnormal halved, the largest doubled), and a program may print them. D-031 is
the one recorded departure it does not steer around: ctang prints a NaN as `nan`
or `-nan` by the sign bit, lang-tang as `nan`. The comparison with ctang
(`oracle::agree_reading_nan`) reads ctang's `-nan` as `nan` anywhere in its
output (a float prints with no delimiter around it, and the generator makes no
string with `nan` in it, which the generator test checks), reads a NaN's sign away
from a float result, and compares every other byte exactly; the corpus comparison
stays strict so the program that shows D-031 keeps diverging. `sanf` is
`san`'s twin that lets a NaN through to a string. A second comparison needs no
ctang: the same programs run by the interpreter and with every function compiled
at its first poll, with the frame observer attached, so that the first polls'
float slots and variables are compared by their bits (any two NaNs agree) as well as
the output and the result (`FuzzDiff.TiersAgree*`; `FUZZ_DIFF_COUNT` makes
`TierCampaign` run more). It is shown to fail on a planted one-ulp
difference, a NaN where a number belongs and an altered output, and, since story 7
compiled float arithmetic, on the same three defects planted in the compiled
emission itself (32 to 34 below). Two more families run the same comparison on
programs built for compiled floats: `generate_carry` (floats held raw across loops
and calls, consumed in every way) and `generate_arith` (`ArithBuilder`: float locals
made from literals and casts, `+ - * /`, negation, the six comparisons and the casts
over floats, integers and the special floats, in loops and branches, with divisors
steered to zero, -1, NaN, infinity and the small-integer bounds, and parameters that
are floats, integers, strings, null and a boolean (`true` is an argument, never an operand the generator writes); seeds 1 to 150, and 1 to 40 again under
torture and verify). The arithmetic family also compares the error lists (every error
entered as it is created) and requires every generated shape and every error of the
matrix to have occurred. Neither family is run against ctang.

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

The fuzz seeds cited below are those of the generator as it was before `spec-runtime-float` story 1, which changed its draw sequence (floats and `sanf` were added): the same seed now makes a different program. The corpus programs named in the ledger rows are the reproductions.

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
operation, on a budget of its own beyond the build): 28 rows (27 under the instruments), at least one per
unbounded native, and since story 9 one that calls a host native from a compiled function (it answers a
huge string in an endless loop; the same table is run again with every function tiering up at its first
poll, so the adversarial operations reached from compiled code, and the natives it calls, are held to the
same bound), from `s = "x"; while (true) { s = s + s; }` under 1,000 fuel to
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
| 15 to 24 | the ten of compiled calls, with 31 below the eleventh (the push hook, the deopt hook, the poll helper, the compile hook, the discard), described in the JIT chapter | `testJit_calls`, `testObserver`; 15 and 20 under relocation | see "The baseline JIT" |
| 25 native-record-not-closed | the shared wrapper opens a native's activation record and never leaves it | the native depth a native sees, and the loop of library calls (`testNative_calls`) | `jit.native_depth` is not 0 |
| 26 native-depth-miscount | a native called from compiled code hands back one unit too few of the native-depth budget the open JIT records cost | the depth a native sees, and the budget sweep (`testNative_calls`) | the compiled run prints 2 where the interpreter prints 1 |
| 27 arguments-not-pinned | the wrapper's record leaves the copies of the native's arguments outside its segment | the relocation arm, `testNative_calls` (a native that allocates and then reads its argument) | a poisoned read; passes with relocation off |
| 28 stale-stack-over-a-nested-activation | the interpreter goes on with the pointer to the guest stack's slots it had before a native that re-entered guest code | the nested-activation tests on a stack that moves at every push (`testNative_calls`) | the plain and compiled runs differ |
| 29 unwind-status-read-as-ok | the library-call thunk answers "continue" for the value a runtime poll returns when it orders the run to stop | the run that runs out of budget inside a native's answer (`testNative_calls`) | the plain and compiled runs differ |
| 30 member-load-by-name-not-charged | the thunk of a compiled `.name` on a library adds nothing to the pending fuel | the loops of `use m.inc as f` and `m.inc(s)` over a sub-library (`testNative_calls`) | the plain and compiled runs' fuel differ |
| 31 deep-push-refused | the push hook refuses once the compiled chain is eight calls deep, so every call below that is an exit; output, fuel and frames are unchanged | the long compiled run of `fib` (`testJit_calls`), which asserts zero call exits and a chain deeper than eight | `tests/unit/test_jit_calls.cpp:1107: Failure`, `exits(jit.stats)` is not 0 (the control prints `fib(22): 57096 compiled calls, 0 call exits`) |
| 32 float-add-one-ulp-off-in-the-jit-arm | a compiled float addition gives the bits of a finite sum plus one (the next double up). The text of a float is six decimals, so the output, the result and the inspected text of every slot are unchanged and only the slot's 64 bits differ | the float programs, compared by bits, interpreter against JIT (`testObserver`, `Observer.TheFloatPrograms*`) | `scope program variable r3 float bits: 0x... against 0x...` at the first poll that holds the sum |
| 33 float-subnormal-flushed-in-the-jit-arm | a compiled float product or quotient that is subnormal is zero of the same sign (both print `0.`) | the same | `variable sub float bits` |
| 34 float-nan-where-a-number-belongs-in-the-jit-arm | a compiled float product that is zero is a NaN | the same | a float slot or variable that is a NaN where the interpreter holds a number |
| 35 pool-boxes-not-reported-as-roots | the float boxes made ahead of time for the rebuild are not reported as roots, so the next collection frees them | the rebuild under torture and verify, which reads the frame's floats back by their bits (`testJit_floats`, `JitFloats.ARebuildUnderTorture*`) | the floats the frame holds differ from the constants |
| 36 reservation-short-by-one | an entry or call extends the reservation one cell short, so a rebuild of a frame that holds that many raw floats is refused up front | the pause with k live raw values (`testJit_floats`, `JitFloats.APauseAtAPoll*`) | the context is not paused: the run ended unwound, `rebuild_failures` is 1 |
| 37 raw-word-in-the-root-cell-across-a-collection | `convert` stores the double's bits in the cell the collector reads as a root, and a collection runs before the box replaces it | the same rebuild under torture and verify | `invalid_pointers` is not 0 |
| 38 reverse-with-no-type-test | `reverse` accepts any value as a float | the checked write-back over a hand-made `F64` site (`JitFloats.ReverseFits*`) | a small integer is written where a raw double belongs |
| 39 pop-does-not-retract | the pop hook leaves the callee's extension in place | the capacity at the caller's polls (`JitFloats.AGuardThatFails*`, `ManyCompiledCalls*`, `ANestedEntry*`) | the capacity grows with every returned call |
| 40 unwind-converts | the deopt hook does not skip the compiled frames of an unwinding run | the unwind with floats live (`JitFloats.AnUnwindWithFloatsLive*`) | `values_boxed` is not 0 |
| 41 box-at-every-slow-poll | the poll helper allocates a float box on every slow path | the allocation slope with a handler pending (`JitFloats.TheSlopeIsStillZeroWithAHandler*`) | the allocation count grows with the loop length |
| 42 merge-conflict-ignored | a merge of unequal kinds is not noticed, so the first way's kind is kept | the merge programs against the interpreter (`JitFloats.AMergeOfUnequalKinds*`) | the output differs |
| 43 float-constant-through-32-bits | the immediate of a float `CONST` is truncated to its low 32 bits | the pause with live raw values, read back by their bits | the floats the frame holds differ from the constants |
| 44 pool-not-let-go-at-host-return | the run's end leaves the pool rooted, so a paused execution reports more root slots than a fresh destination | the pool test: empty pool at a pause, and a snapshot taken there restores (`JitFloats.ThePoolIsLetGo*`) | `b.pool` is not 0 |
| 45 failed-push-keeps-the-extension | a refused stack push does not give back the extension the push hook made | the failed-push test (`JitFloats.AFailedStackPush*`) | the capacity right after the refusal is one callee too large |
| 46 pool-box-reported-through-a-copy | each pool box is visited through a copy, so a collection that moves it leaves the pool the old address | the relocation arm (`testJit_floats` under `GRHEAP_RELOCATE=1` and torture); invisible without relocation | `invalid_pointers` is not 0 |
| 47 failed-rebuild-leaves-raw-words-in-the-guest-frame | before the rebuild the deopt hook copies the compiled frame's words, raw doubles included, into the guest frame, which the injected failure leaves there | the failed-rebuild test (`JitFloats.AFailedRebuild*`): the seam checks the guest frames' value slots for a word that looks like an object and is not in the heap | `gltang_vm_jit_test_stray_words` is not 0 (a collection cannot see it: a guest frame paired with a compiled frame is not scanned) |
| 48 float-divide-guard-tests-the-dividend | the guard that a float division's divisor is not zero reads the dividend, so `x / 0.0` is an infinity where the interpreter makes "divide by zero" | the error rows (`testJit_arith`, `JitArith.EveryCaseTheInterpreterTurnsIntoAnError*`) | the compiled and interpreted runs' keys differ |
| 49 integer-operand-converted-unsigned | a small integer read as a double is converted with `CONVERT_U` | the mixed rows (`JitArith.MixedIntegerAndFloat*`) | a negative integer becomes a number near 2^64 |
| 50 unbox-with-no-kind-test | the unbox loads the double field of any non-null pointer, with no test that its kind is FLOAT | the string, array and boolean operand rows (`JitArith.AFloatReadFromAValueWord*`, the error rows) | a string is added as a double instead of concatenated |
| 51 ge-and-le-as-the-negation-of-lt-and-gt | a float `>=` or `<=` is "not `<`" or "not `>`" | the NaN comparison rows (`JitArith.ANaNHeldRaw*`) | a NaN compares true |
| 52 as-int-upper-bound-inclusive | the range guard of a truncation admits 2^59 | the cast rows (`JitArith.ACastToIntOfADoubleInside*`) | the tagged word overflows where the interpreter makes a boxed integer |
| 53 integer-quotient-not-range-guarded | the quotient of `DIV_S` is tagged with no range guard | the integer division rows (`JitArith.IntegerDivisionAndModulo*`, `AnIntegerResultOutside*`) | `SMALL_MIN / -1` overflows the word |
| 54 result-slot-written-before-the-second-guard | a float operation with a raw first operand writes its operand slot before the second operand's unbox guard | the string-operand-after-a-raw-operand row (`JitArith.AGuardThatFailsInTheMiddle*`) | the frame the interpreter resumes has the first operand changed |
| 55 mixed-subtraction-operands-swapped | `FSUB` and `FDIV` are emitted with their operands in the other order | the mixed rows (`1.5 - a`, `a / 2.0`) | the printed values differ |
| 56 float-operation-adds-no-fuel | a compiled float operation charges nothing | the fuel-parity test (`JitArith.FuelAndPolls*`) | the compiled run's fuel is less than the interpreter's |
| 57 arithmetic-on-raw-operands-is-an-exit | `reads_raw` says yes for the arithmetic and comparisons, so a raw operand exits as before story 7: no output differs | the allocation slope and the exit counts (`JitArith.ALoopOfPlusMinusAndTimes*`, `TheSlopeIsStillZero*`) | the allocation count grows with the loop length |
| 58 integer-converted-from-its-tagged-word | the integer is converted with no `SAR 4`, so the double is `n << 4 \| 1` | any mixed row | the printed values differ |
| 59 unary-minus-is-zero-minus-x | the negation of a raw float is `0.0 - x` | the -0.0 row (`JitArith.UnaryMinusOfARawFloat*`) | `-(0.0)` prints `0.` where the interpreter prints `-0.` |
| 60 unbox-with-no-pointer-test | the tag and null test before the load of a float object's kind is dropped, so null, a boolean or a function value is dereferenced | the null, boolean and error operand rows (`JitArith.AFloatReadFromAValueWord*`, the error rows) | the process is killed by SIGSEGV |
| 61 as-int-lower-bound-too-wide | the truncation's lower bound is -2^62 (the verifier accepts it) | the cast rows (`JitArith.ACastToIntOfADoubleInside*`) | -2^59 - 128 overflows the word |
| 62 nan-divisor-read-as-zero | the divide guard is "< 0 or > 0", which a NaN fails | the division by NaN row (`JitArith.ADivisionByNaNOrInfinity*`) | `x / nan` exits where it should be compiled |
| 63 modulo-zero-divisor-guard-removed | `REM_S` with no zero-divisor guard: the verifier refuses the function | the modulo rows (`JitArith.IntegerDivisionAndModulo*`) | `compile_failures` is not 0 |
| 64 neg-compare-cast-mod-div-add-no-fuel | `NEG`, the float comparisons, both casts, `%` and integer `/` charge nothing | the fuel comparison inside every `expect_same` (cast, division, fuel-parity rows), each removal also checked by hand | the compiled run's fuel is less than the interpreter's |
| 65 bench-reference-one-off | the C reference of the benchmark harness's float loop is 1.0 off (the defect is in `bench/bench.c`, not in the library) | `bench --smoke` over the fresh-engine and the steady-state selection of that program, both of which must abort | the harness aborts (`setup failed`, exit 134) where the control passes |
| 66 every-push-looks-the-callee-up | the push hook looks a callee's raw floats up for every compiled call, as the first float story did, even when no compiled function holds any | the float-free call test, which counts the lookups across `fib(15)` (`JitFloats.ACompiledCallWhoseCallee*`) | `extension_lookups` is not 0 |
| 67 every-extension-is-recorded | the extension helper records an extension of nothing (reached by every entry, with `max_converting` 0), alone: the push path is as committed | the float-free call test, counting the records across `fib(15)` while the lookups stay 0 | `extension_lookups` is 0 and `extension_records` is not |
| 68 bench-integer-reference-one-off | the integer reference of the division loop is 1 off (in `bench/bench.c`) | `bench --smoke --only jit-intdiv-div3`: the integer path of the answer check | the harness aborts where the control passes |
| 69 bench-steady-expectation-one-off | the steady-state cases expect the answer for n + 1 and the fresh-engine cases do not | `bench --smoke --only steady-float-cmp`: the steady-state answer check alone | the harness aborts |
| 70 bench-family-expectation-one-off | the cast family's expected value is 1 off | `bench --smoke --only float-entry-cast-L1`: the family cases' exact expectation | the harness aborts |
| 71 bench-compiled-row-is-interpreted | the compiled rows run at a threshold of zero | `bench --smoke --only jit-float-cmp`: a compiled row must have compiled and entered | the harness aborts |
| 72 bench-discard-expectation-wrong | a function that fails for real is expected never to be discarded | `bench --smoke --only steady-float-exit-nanint`: the number of functions discarded | the harness aborts |
| 73 expected-exit-counts-toward-the-limit | the deopt hook counts the exit that boxes a raw float toward the discard limit | the looped float-returner that must stay compiled across fourteen calls (`JitArith.AFunctionWithALoopThatAlwaysEnds*`) | `functions_discarded` is 1, not 0 |
| 74 static-decline-ignores-the-loop | the compile declines a function whose every path ends at that exit without asking whether it can loop | the same test | `functions_declined_expected` is 1 and `exits_expected` is 0 |
| 75 static-decline-ignores-a-real-exit | the decline looks only at the returns, not at the real exits | the function with a real exit path beside a float return (`JitArith.AFunctionWithARealExitOnOnePath*`) | `functions_declined_expected` is 1 |
| 76 bench-expected-exit-count-wrong | the exits that only box a raw float are expected one more than the calls | `bench --smoke --only steady-float-return-L1`: `exits_expected` is the number of calls | the harness aborts |
| 77 bench-decline-not-expected | a loop-free float-returning function is expected to compile | `bench --smoke --only float-return-L0`: a function the compile declines must be seen declined | the harness aborts |
| 78 bench-counted-exit-count-wrong | the top level's exit is expected not to count | `bench --smoke --only steady-float-entry-int-L1`: `exits_counted` is exactly the exits that count | the harness aborts |
| 79 static-decline-ignores-a-normal-return | the decline counts only the real exits, not the paths that return normally, so a loop-free function with a float path beside an integer return is declined | the function with a float path and a normal integer return, which must be compiled and entered (`testJit_arith`) | the function is declined and never runs compiled |
| 80 exit-sites-from-the-functions-record | the deopt hook reads an exit's flags from the function's record, whose code is gone if the function was discarded while the record was open | the discard-while-a-record-is-open test (`testJit_floats`) | a designed exit taken from such code is counted |
| 81 bench-exit-kinds-sum-omits-remembered | the harness's exit-kinds invariant leaves out the remembered call exits | `bench --smoke --only float-caller-declined`: `deopts` is exactly the sum of the classified exits | the harness aborts |
| 82 bench-declined-callee-entries-ignored | the harness expects a declined function's callee never to be entered | `bench --smoke --only float-arg-L0`: a declined function is not entered, its callee is, exactly | the harness aborts |
| 83 every-compiled-call-looks-the-callee-up | the push hook looks the callee's function record up for every compiled call (as the run-time slot resolver and the per-call signature check did) | `JitCalls.ACompiledCallLooksNoFunctionUp...`: `record_lookups` is the same for fib to depth 8 and to depth 20 | `large.stats.record_lookups` is not equal to `small.stats.record_lookups` |
| 84 container-held-across-a-childs-allocation | the materializer keeps the array or map it is filling in a C variable, not a temporary root, so the allocation of a child can collect it | the descriptions read under torture and verify (`testHost_values`) | the process is killed by SIGSEGV (a store into a collected object) |
| 85 raw-word-in-a-temp-cell | a map entry's value is held across the allocation of its key as the raw bits of a float, not the tagged word | the numbers in a map under torture and verify (`testHost_values`) | the process is killed by SIGSEGV |
| 86 string-tag-dropped | the materializer makes every string TRUSTED, so html, percent and javascript strings are no longer escaped | the rendering differentials (`testHost_values`) | `both.described.rendered` differs from the reference |
| 87 map-key-tag-dropped | every key is made TRUSTED | the map-key test (`testHost_values`) | `described.rendered` differs from the reference |
| 88 materialization-not-charged | the pacer adds nothing, so a copy of any size costs the run nothing | the fuel a long copy is charged, and the budget that stops it (`testHost_values`) | `unlimited.fuel` is 7 against a baseline that needs 707 |
| 89 in-run-arena-from-the-process-allocator | a description a native or factory builds during a run takes its memory from the process | the memory-budget test of a factory (`testHost_values`) | `hog.refused` is `GLTANG_OK` where `GLTANG_ERR_OOM` is expected |
| 90 materializer-depth-unchecked | the materializer recurses as deep as the description says | the over-deep description reached through the test hook (`testHost_values`) | the result is an array, not `Recursion Limit Exceeded` |
| 91 builder-depth-unchecked | attaching a node never asks whether the nesting is within the limit | the depth-limit tests (`testDescription`) | the push past the limit returns `GLTANG_OK` |
| 92 builder-node-limit-unchecked | making a node never asks whether the description is full | the node-limit test (`testDescription`) | the fourth node of three is made |
| 93 builder-byte-limit-unchecked | adding a string, a key or a node never asks whether the bytes fit | the byte-limit tests (`testDescription`) | the node past 48 bytes is made |
| 94 second-parent-allowed | a node that has a parent can be attached again | the one-parent tests (`testDescription`) | the second push of a node returns `GLTANG_OK` |
| 95 reach-cached | the first reach in an execution makes the value and every later reach gets the same object | the fresh-copy tests: two reaches, a reach in a loop, a second execution (`testHost_values`) | the loop prints 1234, not 1111 |
| 96 no-ancestor-check | the walk up never compares an ancestor with the node attached, so a cycle can be built | the cycle tests (`testDescription`) | an ancestor is attached under its descendant |
| 97 no-self-attach-check | a container may be pushed into itself | the self-attach rows of the cycle test (`testDescription`) | the push of an array into itself returns `GLTANG_OK` |
| 98 materializer-node-limit-unchecked | the materializer never counts the description's nodes against its limit again | the description past its own node limit, through the test hook (`testHost_values`) | the result is an array, not `Out of memory` |
| 99 materializer-byte-limit-unchecked | the same for bytes | the description past its own byte limit, through the test hook (`testHost_values`) | the result is an array, not `Out of memory` |
| 100 utf8-unchecked | the builder accepts any bytes as a string or key | the invalid-text tests (`testDescription`) | a text that is not UTF-8 is accepted |
| 101 materializer-node-ceiling-ignored | the materializer holds a description to its own limit and not to the engine's ceiling | the description that claims no limit, against lowered ceilings (`testHost_values`) | the result is an array, not `Out of memory` |
| 102 materializer-byte-ceiling-ignored | the same for bytes | the same | the same |
| 103 ref-of-another-description-accepted | a ref is checked against the node count only, so a ref made by one description names a node of another | the foreign-ref test (`testDescription`) | the push with a ref of another description returns `GLTANG_OK` |
| 104 persistent-not-rooted | the persistent table is not reported as roots | the persistent handle across collections under torture (`testHandles`) | the handle's value is gone |
| 105 handle-serial-unchecked | a handle is looked up by position alone | the stale-handle test (`testHandles`) | a handle of a returned callback is valid |
| 106 scope-limit-unchecked | a scope takes handles without end | the scope-limit test (`testHandles`) | ten handles made under a limit of four |
| 107 segment-tag-flattened | the segment reader reports every run as TRUSTED | the segment test (`testHandles`) | the html run reads as trusted |
| 108 persistent-table-leaked | the persistent arrays are not freed at destroy | the context's leak check (`testHandles`) | `live_blocks` is not zero |
| 109 persistent-limit-unchecked | a host can promote without end | the persistent-limit test (`testHandles`) | the fifth promotion of four succeeds |
| 110 no-gc-function-collects | `gltang_handle_array_get` runs a collection | the label gate (`testHost_callbacks`) | a `@no_gc` function moved the collection counter |
| 111 held-libraries-leaked | libraries made into values are never released | the count of cutil allocations across a run (`testHandles`) | allocations minus frees did not return |
| 112 owned-members-leaked | native records are never freed | the context's leak check (`testHandles`) | `live_blocks` is not zero |
| 113 frame-pop-keeps-handles | a popped frame leaves its values on the temporaries | the frame test (`testHandles`) | a build with frames runs out of handles |
| 114 answer-read-after-the-scope-closes | the result is read after the scope is dropped | the answer tests (`testHandles`) | a handle answer is `Host function failed` |
| 115 stored-container-aliased | a pushed container is stored itself | the copy test (`testHandles`) | the stored copy grew with the original |
| 116 scope-serial-reused | every scope takes serial 1 | the stale-handle test (`testHandles`) | a new scope's serial equals the old |
| 117 persistent-reported-through-a-copy | the collector is shown a copy of each persistent value | the persistent handle under relocation torture (`testHandles`; relocation arm only) | not seen without relocation |
| 118 template-output-delivered | the delivery does not check that the buffer is the main activation's | the template output test (`testHost_context`) | a template's text arrives twice |
| 119 delivered-point-not-advanced | a delivery does not record how far it got | the delivery test (`testHost_context`) | each print repeats everything before it |
| 120 refused-print-not-rolled-back | a refused print stays in the buffer | the refusal test (`testHost_context`) | the buffer holds `abc` for a print that failed |
| 121 language-layer-before-the-program | the layer is searched before the program's libraries | the precedence test (`testHost_context`) | the layer's `pages` hides the program's |
| 122 language-layer-leaked | an execution never gives back the layer it took | the count of cutil allocations (`testHost_context`) | a replaced library is not freed |
| 123 language-layer-read-afresh | a `use` reads the layer the process has now | the replacement test (`testHost_context`) | a replacement reaches a running program |
| 124 language-layer-not-sealed | the layer is retained and not sealed | the readable-layer test (`testHost_context`) | `gltang_library_sealed` says false |
| 125 host-context-shared | the context is one process-wide variable | the two-executions test (`testHost_context`) | two executions see the last one set |
| 126 render-ignores-tags | every run is rendered as trusted | the render test (`testHost_context`) | `<` is not escaped in an html run |
| 127 concat-loses-second-runs | the second string's runs are not put on the joined string | the concat test (`testHost_context`) | the html half renders as trusted |
| 128 substring-one-too-many | a substring ends one grapheme late | the substring test (`testHost_context`) | the substring carries a grapheme it was not asked for |
| 129 value-factory-without-context | the description a value factory fills has no context | the factory test (`testHost_context`) | the factory reads NULL |
| 130 sink-may-reenter | the sink is called without the host-function flag | the re-entry test (`testHost_context`) | the sink destroys the execution under the run |
| 131 snapshot-delivery-point | applying a snapshot leaves the delivered point at zero | the snapshot delivery test (`testHost_context`) | the old output is delivered again |
| 132 refused-constant-print-not-rolled-back | a refused print of a constant (template text) stays in the buffer | the template refusal test (`testHost_context`) | the buffer holds the refused text |
| 133 language-layer-unlocked | a reader loads and retains the layer without the spin flag | the threaded layer test under ThreadSanitizer (`make check-planted-tsan`; TSan only, not in the quick set) | a data race on the slot |
| 134 snapshot-layer-3-is-the-built-ins | a snapshot's layer table maps the language layer to the built-ins | the snapshot-of-a-layer-member test (`testHost_context`) | a native held from the layer cannot be taken |
| 135 language-layer-not-retained | an execution takes the layer without a reference | the library-freed-with-the-last-execution test (`testHost_context`) | a replaced layer is freed under a running program |
| 136 delivered-before-the-print-is-whole | the sink is given a print before it is known to have finished | the failing-print sweep (`testHost_context`) | the sink holds bytes the buffer rolled back |
| 137 decline-answered-with-an-error-value | a declined operation makes the language's error value, which is dropped | the decline allocation count and the error-hook tests (`testCustom`) | the decline that the other side answered made an object |
| 138 reflected-call-not-swapped | the right operand's operation is called with the left operand as `self` | the reflected-call tests (`testCustom`) | `3 + money(5)` is not money 8 |
| 139 reflected-call-after-an-answer | the right operand is asked even when the left answered | the left-operand-answers test (`testCustom`) | the log holds two calls for one answer |
| 140 string-rule-answers-a-custom-operand | `+` with a string runs the string rule for a custom other side | the built-in-on-the-left test (`testCustom`) | `"xyz" + money(1)` is `Not supported`, not money's answer |
| 141 left-operand-never-consulted | the reflection rule asks only the right operand | the left-operand-answers test (`testCustom`) | the one call is the reflected one |
| 142 slot-not-traced | the custom heap type traces none of its slots | the slot tests (`testCustom`, a cycle's control and a slot under forced torture) | an object held only by a slot is collected |
| 143 slot-written-without-the-barrier | the handle API stores into a slot with a plain write | barrier-verify in the forced torture tests (`testCustom`) | the verify arm aborts on the slot |
| 144 unattached-type-accepted | `gltang_handle_make_custom` takes a type of a registry nobody attached | the unattached-type test (`testCustom`) | the instance is made |
| 145 registry-not-sealed-on-attach | attaching a registry leaves it open | the seal test (`testCustom`) | `add` after the attach succeeds |
| 146 unsafe-registry-attached-twice | the binding of a thread-unsafe registry is never checked | the binding test (`testCustom`) | a second context attaches it |
| 147 type-identity-by-name-only | two types are the same when their names and versions are | the identity test (`testCustom`) | money version 1 of two registries is one type |
| 148 equality-not-identity | `==` nobody answered is `Not supported` instead of identity | the equality tests (`testCustom`) | `p == p` is an error |
| 149 operation-without-the-host-flag | a type's operation runs with `in_host` clear | the host-function test (`testCustom`) | a setter called from the operation succeeds |
| 150 error-operand-asked-of-the-type | an operator other than equality asks the type when the other operand is an error | the error-operand test (`testCustom`) | `shy() < (1 / 0)` is `Not supported`, and the log holds the call |
| 151 element-equal-error-read-as-false | an error that an element's `equal` answers makes an array comparison false | the container-comparison error test (`testCustom`) | `[f] == [g]` is `false`, not the error |
| 152 print-skips-its-fuel | the `PRINT` thunk adds no cost | the loop of prints (`testJit_templates`) | the fuel differs from the interpreter's |
| 153 print-leaves-its-operand | the `PRINT` thunk's value is discarded, so the slot keeps the operand | the loop of prints (`testJit_templates`) | the function returns 99, not null |
| 154 long-text-printed-without-pacing | the `PRINT_CONST` thunk prints in pieces through sinks of their own, none reaching the pacing interval | the large-print test (`testJit_templates`) | no pacing poll, less fuel, and the scope that must run out inside the text does not |
| 155 print-position-not-stored | the call site stores no position before a print | the refused delivery and the large print (`testJit_templates`) | the error's origin is another line; the polls' identities differ |
| 156 interpreted-print-not-at-the-print | the interpreter's prints leave the frame where the last poll left it | the frame differential over the output programs (`testObserver`, `Observer.ACompiledPrint*`) | the poll identity, `pc` and `sp` of a poll inside a long print differ from compiled code's |
| 157 compiled-print-not-at-the-print | the print thunks leave the guest frame's identity at the last poll | the large-print test (`testJit_templates`) | the identity of a poll inside the print differs |

32 to 34 were planted in the interpreter's arithmetic (under `exec->jit_threshold`) while the compiled float arithmetic of `spec-runtime-float` story 7 did not exist, and stood for it: each is a tier-dependent float result, which is what a wrong compiled operation would be. Story 7 moved each into the compiled emission, with the same number, name and instrument, so that they are caught by the real thing (a compiled add no longer runs `arithmetic`, where the old plants lived). The same three kinds of difference are also planted in a recorded trace (`ObserverFails.AOneUlp*`, `ASubnormal*`, `ANaNWhere*`), where the test shows that the text channel alone does not see the first two, and a pair of programs one ulp apart whose text is identical (`TwoRunsOneUlpApart*`) is shown to diverge on the bits and nowhere else.

48 to 64 are the compiled float and division operations of story 7 (60 to 64 added at review: the pointer test, the cast's lower bound, a NaN divisor, the `%` guard and the fuel of the other operations; 60 is a wild load, so it is caught by the process being killed, not by an assertion); 57 is the one performance defect (nothing is wrong in any output; the allocation slope is what fails).

35 to 47 are the carried float of story 6. The story's third defect, a raw slot written before its conversion finished, is 37 in the form the collector sees (a raw word in the conversion's root cell across a collection) and 47 with the failing-rebuild seam (raw words copied into the guest frame before a rebuild that is then refused); the core's own ordering is tested in runtime-core, `test_rebuild.cpp`.

152 to 157 are the output operations compiled of `spec-template-compiled` story 1: four in the thunks and the emission, 156 in the interpreter (seen only by the JIT arm, which compares a poll inside a print with compiled code's) and 157 its compiled twin, seen through the frame header rather than the walk.

137 to 151 are the custom kind, its descriptor and its registry of story 5 (all in `testCustom`, run plain: the tests that need the collector force torture and verify on themselves; 143 is caught by the verify arm aborting). 118 to 136 are the host context, the language layer, the output sink and the string helpers of story 4 (all in `testHost_context`; 133, the layer's slot read without its lock, is seen only by ThreadSanitizer, so it is not in the quick set: `make check-planted-tsan` applies it to a copy and requires the TSan run to report the race); 104 to 117 are the handles of story 3 (117 is run by the relocation arm); 84 to 103 are the host-described values of `spec-tang-host-extensibility` story 2, all in the library and none in the JIT. 84 and 85 are the two that only a collector finds (the plain run of the same test passes: the torture and verify arm is forced inside the tests that carry them, so `make test` runs them in every arm); each of the others is a rule of the builder (91 to 94, 96, 97, 100 and 103) or of the materializer's second look at it (90, 98, 99, 101 and 102), stated by a test from the outside. 84 and 85 are caught by the process dying, not by an assertion: a store into a collected object, or a number read as a pointer.

`make test` runs the quick set, 03 to 09, 11, 12, 16 to 19, 21 to 26 and 28 to 45 and 47 to 116, 118 to 132 and 134 to 157 (`check-planted-quick`, about two minutes with the
first build of the copy); 01 and 02 are part of `make test-torture`
(`check-planted-slow`, 6 seconds once the copy is built). `make check-planted`
runs the hundred and thirty-nine that need neither a relocation heap nor ThreadSanitizer (01, 02 and the quick set; 10 and 18 are retired, 133 is `make check-planted-tsan`'s). 13, 14, 15, 20, 27, 46 and 117 are run by `make check-planted-relocate`, which the
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
each family: growing an array or a map
(`array_new`, `array_grow`, `map_new`, `map_set`, and the stores that call them:
a value stored beyond the end of an array that is itself the value, a boxed
integer stored while the array grows); the deep copy and the comparison of
containers; the operators that build arrays (concatenation, repetition, slicing,
`+` with a string); every string operation that copies in chunks (concatenation,
substring, slice, retag, render); the sinks (printing and rendering a container
or a string into the output); and a native bound to a value.

What `testRelocate` shows for each. The families that allocate (growing, copying,
the array operators, the string operations' result, the native) each have a case
that fails with the fix removed; that was checked against the parent of each fix
commit, and every case crashes there. The loops that poll as they copy, and the
sinks, move an object only at a poll, and a poll collects only when the memory
budget asks it to reclaim; a test cannot set a budget to a program, so one case
(`TheLoopsThatPollAsTheyCopy...`) posts a request that never ends, which makes
every runtime poll a slow one that runs the heap's handler, which under torture
collects, which under relocation moves. It runs a multi-chunk concatenation, a
slice with a step, a substring, retag and render, an array grown and compared at
length, and the printing of long strings and of a container of them, each
compared with the ordinary heap's run. Deleting the re-read after the poll in
concatenation, in slice, in the sink's string copy, in `array_grow` and in
`equal_rec`, one at a time, makes it die (a stale read of a large object's
unmapped old mapping), so those re-reads are run and needed. The rule is now: a
value held across a GC point is pushed as a
temporary (`gltang_vm_temp_push`) and read again from it (`gltang_vm_temp_at`),
and a pointer into an object is derived again after every poll; `temp_at` and
`temp_release` assert (in a build without `NDEBUG`) that the index or mark is
within the temporaries. The interpreter's own loop already reloaded the frame after
every GC point and the baseline JIT's poll copies the guest frame back after the
collector has rewritten it, so neither needed a change. The arm found no address
used as a key in the engine; that is what it found, not a proof of absence.

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

At the end of milestone 1: 29 rows, **26 recorded, 3 fixed, 0 open**, and the
ledger was closed. Categories of the recorded rows: 17 ctang defects, 5 limits,
3 error-reporting, 1 rng. ctang stays in `libraries.txt` and the oracle gate:
whether it still needs to be the oracle is a later decision (AD-16 retirement).

**Since 2026-10-16 the ledger is not closed.** The rows had since grown to 31
(the float spec's D-031 among them), and D-032 to D-056 are a new category,
`capability-removed`: one row for each host-visible capability that ctang
offered and lang-tang lacks, found by comparing the two engines' host surfaces
(`planning/tang-host-extensibility.md`, section 3). They are `open` until
`spec-tang-host-extensibility` restores each one, and a row becomes `fixed` when
its test passes: D-032 to D-038 (story 5) and D-048 to D-056 (stories 2 to 4)
are, and D-039 to D-047 are open. Because ctang retires only when the ledger is closed (spine
AD-3), ctang stays while a capability it offered is missing. The milestone-1
claim above that the ledger was closed was true of the divergences it
compared; it did not compare what a host could do, which is how these were lost
without a decision.

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

A small baseline JIT (story 15, CAP-10, AD-9; calls between compiled functions,
spec-runtime-calls story 8, AD-28) behind a build option, `JIT=yes|no`.
It tiers up a function that is hot, compiles the part of it that is cheap and
exact to machine code through `runtime-jit`, calls other compiled functions
directly, and leaves compiled code for the interpreter, in the guest frame
(rebuilt, for a chain), whenever it meets anything else. It is the
first client of `runtime-jit` and of what `runtime-core` gained for it (`a/code.h`,
reference-counted compiled code; `a/deopt.h`, a reader and writer of a native frame
by its metadata; and, for calls, the registry of code, entry slots, the precise
walk and the chain rebuild). `lang-tang`'s observable behaviour is the
same on both tiers, by construction and by test: no divergence-ledger row exists
for the JIT, and none may be added.

**Calls, and where the protocol is written (CAP-6).** The call protocol is built across
five libraries and written once in each, for its own part, with a pointer to the others.
`runtime-jit` has the convention between compiled functions, the call sequence and the
hooks' contract ("Calls between compiled functions"); `runtime-core` has the walk of a
chain, the rebuild of all of it, the native stack in bytes and the entry slot's refusal
("A, part 5"); `runtime-heap` has what the collector does under a compiled chain and
the relocation torture that proves it ("Calls"); `runtime-debug` has the pause over a
chain ("Calls"); and this section has the engine's side: which calls are compiled,
the hooks, the exits and the discard limit, and the figures ("Calls, measured").
**Calls as deopt exits were milestone 1's defect** (Corey, 2026-10-05): the baseline
treated every `CALL` as an exit, so a compiled function left compiled code at each call
and its callee re-entered its own code, which made call-heavy code slower with the JIT
than without it. The rule behind it ("no JIT frame calls a JIT frame") was story 15's,
not the spine's. The alternatives this library rejected are under "Why this and not
something else", below.

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

**A compiled function is callable (AD-28).** It takes the callee's parameters as
references and one hidden trailing flag, has an internal entry that other compiled
functions call, and is registered with the context (`grcore_code_register`) from
the moment it is installed. Every function the execution has named has one entry
slot (`GRCORE_EntrySlot`), made when the function is first named (by a caller's
compile or by its own) and kept for the execution; the slot's state mirrors the
function's: empty while it is cold, its code once it is compiled
(`grjit_entry_slot_install`, which refuses code built for another token or another
parameter count), refused once it can never be (declined, failed or discarded).
The token is the function word and the count is `parameter_count + 1`.

**Entry from the interpreter.** Compiled code is entered right after a function's
entry `POLL` (bytecode offset 0) has returned continue in the interpreter, with the
flag 1: the function starts "after the entry poll", at index 1 with an empty
operand stack. The guest frame was pushed by the interpreter exactly as for any
call and is the frame a pause shows. A tier-up in that very poll is entered in
that invocation (documented behaviour of `gltang_execution_set_jit_threshold`; it
is what makes a threshold of 1 cover the top level). Entering records a
`GRCORE_ACTIVATION_JIT` record, which draws on the native-depth budget (AD-21): if
that is full the function is not entered this time. A compiled call enters the
callee with the flag 0, and the callee's prologue then makes its own entry poll
(one body, one compare: the prologue branches on the flag), so exactly one entry
poll is made per activation, by the tier that made the call.

**A call.** `CALL n` compiles to a call when its callee operand was produced, on
every path to it, by one `LOAD_GLOBAL g` or one `LOAD_LOCAL s` (the stack analysis
tracks the producing instruction of each operand position and refuses a merge of
two), when the declaration scan names that slot's function `k` (the triple `FUNC
k; STORE_GLOBAL g; POP` in function 0 of the program, or the same with
`STORE_LOCAL s` in the same function: the shapes the compiler emits for a
declaration), and when `k` takes `n` parameters and its frame fits a compiled
function. The site loads the callee value, guards it equal to the function value
`k` (a mismatch is a guard exit, counted against the caller), and makes one
`CALL_SLOT` through `k`'s slot with the token and the `n` operands plus the flag 0.
Everything else stays an exit, with the same discard accounting as any exit: a
native, a template, a value the scan cannot name (a parameter, an array element, a
global with no declaration), an argument-count mismatch, a callee with more than
fifteen parameters or too many slots. A context with no native-stack byte budget
compiles no call site at all: recursion measured in bytes has nothing to be
measured against, and a fault is never an acceptable result.

**Library calls and loads (spec-runtime-calls story 9, CAP-7).** A `CALL n` of a library
native, and the `USE` and `.name` that produce it, compile as `GRJIT_OP_CALL_NATIVE`
(runtime-jit's registered, typed native call: a GC point, an exit state before it, a status
and the state after it) to thunks in `src/jit/natives.c` that are the interpreter's own
operations: `gltang_vm_call_native` (above), `gltang_vm_resolve` and `gltang_vm_op_attr`, called
as the interpreter calls them, so the value, the allocation, the fuel and the error text are
one code path and a load gives whatever the libraries hold *now*. The table (one per execution,
`GRJIT_NativeTable`) holds a call thunk for each argument count from 0 to 15 (the callee is the first
parameter), the `use` thunk and the `.name` thunk, each with a status; a call thunk declares the
re-enter flag and every thunk 16 KiB of native stack (what a thunk reaches includes an allocation, so a collection, and a runtime poll with its handlers, none of which checks the budget), which the call site checks against the
native-stack budget before it calls (an exit, not counted toward the discard limit, with nothing
charged). What is *static* is only which sites compile: the producer analysis names the
instruction that made the callee, and a small scan finds the shapes `compile_use` emits
(`USE k; STORE_x s; POP`) as the function scan finds a declaration's, so a callee read from a
global or a local that one `use` binds, or from `.name` of a library that one `use` binds, is
resolved against the libraries now to the member it names. A site compiles when that member is a
host function that is not flagged resumable (and, for a load, a host function or a library). The
call site then guards the callee value against that member (a leaf that is not a GC point: the
value is a native of that member and not an engine builtin); a mismatch is a guard exit and counts
toward the discard limit, so a variable that is reassigned costs one exit and not a wrong call. A load
needs no guard: it is the interpreter's operation, called. Everything else stays an exit as before:
a load that does not yield a host function or a library, the engine's builtins (`random.seeded`,
`rng.set_seed`), the poll operations of `natives.def`, templates, a call with more than fifteen
arguments, a value the scan cannot name. The call's fuel is charged by the thunk after the site's
checks have passed (as the push hook charges a guest call's), so an exit before the call charges
nothing and the interpreter charges it when it makes the call.

A thunk returns the value and a status. `GLTANG_V_UNWIND`, which a runtime poll returns when it orders
the run to stop, is `GRJIT_NATIVE_UNWIND`: the site exits with the state after the call, the deopt
hook skips every compiled frame of the run as for a poll's unwind (cause `NATIVE | 2`) and the
interpreter's `unwound` takes over. A test native may ask compiled code to leave (`GRJIT_NATIVE_DEOPT`,
cause `NATIVE | 1`): the whole chain is rebuilt with the call done and its value in place, and the
interpreter goes on after it; the native ran once. Neither status counts toward a discard limit. A
backend that refuses natives, or an execution whose table could not be made, leaves the sites as
exits and counts them (`native_sites_unsupported`), never a fault.

**The output operations (spec-template-compiled story 1, CAP-1).** `PRINT`, `PRINT_CONST` and
`CLEAR_RESULT` were unconditional exits, and a function whose first operation is one of them was not
compiled at all (the entry check). Template bodies are mostly these three, so a template's function 0
was either declined or left compiled code at its first text, and templates gained nothing from the JIT.
They are now calls of the interpreter's own functions, so there is one implementation of printing:
`PRINT` and `PRINT_CONST` are `GRJIT_OP_CALL_NATIVE` to two more thunks of the table (`print`, which
takes the value, and `print_const`, which takes the constant's string block), which call
`gltang_vm_op_print` and `gltang_vm_print_constant`; `CLEAR_RESULT` is a call that is no GC point to
`gltang_jit_clear_result`, which is `gltang_vm_set_result(exec, null, false)` (it enters a lost result
in the error list, which is the execution's own memory, and nothing it does polls or touches the heap,
so a frame state and a stack map would describe nothing it needs). Output, its capture, the rollback of
a print that fails, the sink and its delivery (with `in_host` set, as interpreted), the pacing of a
long print (`sink_pace`, a runtime poll per `GLTANG_POLL_BYTES`) and the error a failed print makes are
therefore the interpreter's. Each thunk charges its operation's cost once, as the `use` thunk does, and
the call site stores the instruction's position first (`emit_site`, the interpreter's `SYNC`), so an
error's origin and a poll's location are the interpreter's. `PRINT` replaces its operand with the
print's value (null, or the error of a print that failed: an error printed is swallowed and the value is
null); `PRINT_CONST` pushes nothing and its value is dropped, as the interpreter drops it; `CLEAR_RESULT`
touches no slot. A thunk's `GLTANG_V_UNWIND` (a pacing poll that stopped the run, a scope that ran out
inside the text) leaves through the chain deopt with the state after the call, as a native's does, and
what the print had appended is already rolled back. A print of a raw float is not given to the thunk:
it is the designed exit that boxes the float (`reads_raw`), so its text is the interpreter's and compiled
code never boxes (AD-27). The two prints compile only where natives can be called (`natives_on`, as
`USE`); elsewhere they stay exits, counted as before. `CLEAR_RESULT` is a plain call and compiles
everywhere. Neither thunk declares the re-enter flag: a sink may not re-enter guest code.

*The frame at a poll inside a print.* A long print polls inside the call, and there the walk shows the
compiled printer's frame at the print: `pc` the print's index, `sp` with its operand, and the poll
identity of the site. The interpreter's `PRINT` and `PRINT_CONST` used to leave the guest frame's `pc`,
`sp` and identity where the last guest poll had left them, so the two tiers showed different frames at
the same poll, and so did the old exit at a print (the rebuild gives the frame the exit site's
identity, which the interpreter then printed under). The interpreter's two prints now stand the frame
at the print before they call (`SAVE_AT_OP` and `grcore_stack_set_identity`, what its `USE`, `ATTR` and
native `CALL` do for the state), and the thunks set the guest frame's identity from the stored position,
because a reader of the frame header (`grcore_context_poll_identity`) sees the guest frame and not the
walk. Neither changes what a program prints, its fuel or its polls; it changes the frame a poll inside
a print sees, in both tiers, to the print's own. The frame differential over the output programs
(`output_cases`, testObserver) and the identity of every poll in `testJit_templates` are the gates
(planted 156 and 157); 156 is in the interpreter, and only the JIT arm can see it.

**The hooks** (`src/jit/hooks.c`). *push* checks its own arguments (a token that
names a function of a loaded program, the count `parameter_count + 1`, one result,
and the flag 0; anything else is refused and counted in `hook_argument_errors`), tests the guest
depth at the interpreter's condition, pushes the callee's guest frame (the
frame-push GC point; it fires no `CALL_REFUSED` of its own: the interpreter makes
the refused call after the exit and fires it once), reads its arguments **after**
the push, writes the header (function, `pc` 0, `sp`, flags = the activation's
depth), the arguments and zeros, sets the callee's identity to `{function, 0}` and
adds the `CALL`'s fuel. *pop* pops the frame. *compile* compiles the callee at its
first call from compiled code, installs it and returns zero; a function that is
declined or fails is marked never-compile, its slot is refused and the call site
exits, remembered. *deopt* walks the chain, sets the identity of every caller's
guest frame from its call site (the interpreter does that at each call; the rebuild
does not), classifies the exit, and calls `grcore_compiled_rebuild`, which writes
every compiled frame of the chain into its existing guest frame. Nothing is
inserted, so no offset moves; each frame then returns `DEOPTED` and nothing else
runs in compiled code. For an unwind verdict the rebuild is told to skip every
compiled frame (compiled code opens no scope, so all of them are inside the one
that is unwinding) and still marks the record rebuilt so that it can be left.
Tang functions have one VM result; the tail hook validates that shape and declines
without changing the guest stack, leaving the call to the interpreter fallback.

**Exits and the discard limit.** An exit at a call site because the callee is
remembered refused, because the push was refused (guest depth, memory) or because
the native stack would run out is not counted toward any function's discard limit:
the callee or the budget caused it. A failed guard, the callee-value guard, an
unconditional exit are counted against the function whose frame is innermost
(a poll's pause or unwind is not: a debugger step or a fuel pause must not throw
away hot code, and an unwind ends the run), and at the eighth that function is discarded: its slot
is refused (so its callers' calls become remembered exits), its range is
unregistered and the cache's reference is dropped. The code is never destroyed
directly: the registry and the slot retire their references and `runtime-core`
frees the code when no JIT record is open, so a frame waiting in a chain still
returns into it. The discard is applied after the entry's record is left.

**What is compiled.** Inline: `POLL`, `LINE`, `POP`, `DUP`, `NULL`, `TRUE`,
`FALSE`, `CONST` (a small integer or a float), `LOAD_LOCAL`, `STORE_LOCAL`,
`LOAD_GLOBAL`, `FUNC`, `NEG`, `NOT`, `ADD`, `SUB`, `MUL`, `DIV`, `MOD`, `LT`, `LE`, `GT`, `GE`,
`EQ`, `NE`, `CAST` (to int and to float), `JMP`, `JMP_FALSE`, `JMP_TRUE`, `AND`, `OR`, `RET` and `CALL` as above, the
library `USE`, `ATTR` and `CALL` described there, and the output operations `PRINT`, `PRINT_CONST` and
`CLEAR_RESULT`.
Each step mirrors the interpreter's own inline path: both operands carry the
integer tag or a guard fails; `ADD`, `SUB` and `NEG` check the result against the
small-integer range; `MUL` needs both untagged operands inside -2^29..2^29-1 so
the product cannot leave the range; `DIV` guards a zero divisor, the verifier's
minimum-over-minus-one pair and the quotient's range (`SMALL_MIN / -1` is 2^59, one
past it), and `MOD` guards a zero divisor; the operand of `NOT`, the jumps, `AND` and `OR`
must be a boolean. An operation with a statically raw operand is a float operation
(below). Everything else (indexing, attributes,
slices, the `SET_*`, `STORE_GLOBAL`, `ADOPT`, `ARRAY`, `MAP`, `ITER_*`,
`DISCARD`, `SET_RESULT`, a `CAST` to bool or to string, a `USE` or `ATTR` that is not a library load, `HALT` and any constant that is
neither a small integer nor a float) is an unconditional exit at that operation. A tagged value is computed on through `GRJIT_OP_BITCAST`
(a `REF` reinterpreted as an `I64`, shifted, added, tagged and reinterpreted back).

**Frame states are exactly the guest frame.** `frame_slots` slots in the guest
frame's own order and no more: the function, `pc` and `sp` are constants, the flags
word is a register holding the activation's depth (loaded once, in the prologue),
a local is its register, an operand slot below the depth is its register, and a
slot above it is a register that holds zero typed as a reference, so it reads as
the zero value slot the interpreter's frame holds (a `DEAD` location reads as a raw
zero, which would be a different frame). The abstract frame reads a compiled frame
by its site, and the descriptor reads every slot through `grcore_frame_slot`, so
a stack trace, a debugger's locals and the frame differential see the same chain
whichever tier made it; that needed `runtime-core` to ask the engine for the scopes
of a compiled frame that stands for a guest frame (it said none).

**Fuel (AD-21)** is the same on every tier and lives in the execution: compiled
code adds each straight run's cost directly to `exec->pending_fuel`, before
anything that can observe it or leave (a guard, a call, a poll, a branch, `RET`),
and flushes it to the context before every poll (`gltang_jit_flush`), as the
interpreter's `POLL` does. A frame state carries no fuel, so an exit and a chain
deopt lose nothing. A guard is tested before its operation's cost is added, so an
operation that deoptimizes has not been charged and the interpreter charges it
when it runs it; a `CALL`'s own cost is added by the push hook once the push has
succeeded, so a refused push charges nothing and a poll in the callee already sees
the call's cost. A budget therefore pauses or unwinds at the same poll with the
same total on both tiers, which the fuel-parity tests check for every budget from
1 to 120 over a three-deep call chain.

**Polls, and the frame at a GC point.** A compiled poll is a flush of the fuel (a
call of a helper that is not a GC point), a load of the request word and a branch;
the poll helper `gltang_jit_poll` is called only when something is pending. It
sets the execution's current location, polls with the same identity as the
interpreter's own poll, and answers 0, 1 (pause) or 2 (unwind). It finds nothing through
its own frame pointer (the JIT module is no longer built with `-fno-omit-frame-pointer`,
and the stamped Makefile flag is gone): the callable function's poll stub stores the
walk start before it calls the helper. **It does not touch
the guest frame**: the polling frame is paired with its guest frame, the collector
updates the compiled frame's references through its stack map and does not scan the
guest frame, so a write-back of the guest copy would put stale references over
updated ones. A pause or an unwind arrives at the entry as `GRJIT_EXIT_DEOPT` with
the cause (1 or 2) in `out[0]`, after the chain has been rebuilt (a pause) or
skipped (an unwind). `GRJIT_EXIT_REBUILD_FAILED` is an error of its own: nothing
was rebuilt, so the run ends as the run's own unwind with every guest frame it
pushed popped and nothing interpreted on them, and it is counted
(`rebuild_failures`).

**`LINE`** loads the execution's `statement_polls` byte at every execution, so a
host that turns the switch on mid-run gets a poll at the next compiled `LINE`, and
with it clear a `LINE` costs a load and a branch and no fuel.

**Ownership.** Compiled code is context-specialised (it bakes in the function word
and the execution's own addresses) and owned by its execution's cache; its pages
come from the group's counting page provider, and the JIT's records and the compile's
own allocations from the group's allocator, so none of it is on the context's meter:
compiled code is the engine's and not the program's, and a memory budget gives the
same verdict compiled and interpreted at every budget (the pages are counted in
`GLTANG_JitStats::code_bytes_mapped` and printed by `--jit-stats`; `runtime-core`
charges its own compiled-code bookkeeping to the group for the same reason, and
the interpreter's first run reserves the activation records' array, which entering
compiled code would otherwise allocate). A compile refused by memory, by a limit or by
`protect` marks the function never-compile, is counted (`compile_failures`) and the
run goes on in the interpreter. The execution's teardown clears every slot,
unregisters every range and releases the reservation (the reservation each entry and
each call extends by the callee's most raw floats at one site, below) before its own
state goes. The emitted metadata has no derived pointer, and its only converting
location is a raw `F64` on a raw slot, which a test asserts.

**Floats carried raw (spec-runtime-float story 6, AD-27).** A float constant is held
in compiled code as a raw double, in a local or an operand slot, and is boxed only
when a frame is rebuilt. Outside compiled code a float is still a boxed
`GLTANG_OBJ_FLOAT`; Tang's value representation did not change. What compiled code
does is the carrying (story 6) and, since story 7, the arithmetic, the comparisons and the
casts over a raw double (the next section):

- *Kinds.* Each local and each operand position has a kind at every point of the code,
  `REF` (a value word) or `F64` (a raw double); a slot that is ever raw has a second,
  `F64`, register beside its `REF` one, and a frame state names exactly one of the two,
  so the other is not live and is not in a stack map. The sources of an `F64` are a
  float `CONST`, a `CAST` to float and an arithmetic result with a raw operand (story 7). A float constant is emitted as the interpreter's double bit for bit (its 64 bits as the
  register's immediate, never through `float` or arithmetic). `LOAD_LOCAL`,
  `STORE_LOCAL`, `DUP`, `POP` and the jumps carry a kind. Every other instruction that
  reads a raw operand (`NOT`, `AND`, `OR`, a jump on it, `RET`, `CALL` with a raw callee or
  argument, a library call or load, `%`, a cast to bool or to string; the operations of the
  next section compute with it) is an
  unconditional exit at that instruction, with the operand in its frame state, and the
  interpreter re-executes it. **Such an exit is designed, not a failure (decided 2026-10-12,
  after the benchmarks of "Floats, measured" showed the discard limit throwing away good code):**
  the site is flagged where it is emitted (`expected_sites` in the code's payload) and the deopt
  hook does not count it toward `GLTANG_JIT_DEOPT_LIMIT`; `exits_expected` counts it and
  `exits_counted` counts the exits that did count. A real failure counts as before: a failed
  kind or type guard (including the unbox guard of a value word that is not a number), a divide
  or modulo by zero, a NaN or out-of-range cast, a result outside the small-integer range, a
  callee-value guard, and an unconditional exit that is not the boxing of a raw float (an
  instruction compiled code does not do, with no raw operand). *The static decline:* a function
  is declined at the compile (never compiled, not a failure, `functions_declined_expected`) when
  the final analysis finds (1) no reachable jump to its own index or an earlier one, (2) no
  reachable `RET` that returns normally, (3) no reachable instruction at which the walk ends for
  a real reason (an unconditional exit that boxes nothing, a call or member load left as an exit),
  and (4) at least one reachable instruction at which it ends at the designed exit. Every path of
  such a function is a few instructions of compiled code and then a designed exit. Edge cases,
  as coded: a guard that can fail (an integer add, an unbox, a divide) is not the end of a path,
  so a function whose successful paths all end at the designed exit is declined even though a
  guard of it can fail and would be a counted exit; a path that ends at an instruction compiled
  code does not do (an array, an index) is a real end, and a branch whose other arm returns
  normally or ends for a real reason keeps the function (tests: a float path beside an integer
  return, a float path beside an array); a function whose paths all end at an operation that
  always errors (`x / 0.0` of a raw operand, `x % 2.0`) is declined only because that operation
  is also an exit that reads a raw operand, not because the error is recognised; any other
  always-erroring operation is not recognised and the function is compiled. A callee that is
  declined makes its compiled callers' calls remembered exits (`call_exits_remembered`, which count
  nowhere), as any function that is never compiled does. **The exits are of exactly one kind:** with
  no rebuild failure, `deopts` is `exits_expected + exits_counted + call_exits_native_stack +
  call_exits_push_refused + call_exits_remembered + native_exits_stack`; `exits_counted` counts the
  exits of a kind that counts toward a function's limit whether or not the function was still
  compiled to be charged (a nested run can discard it while a record is open), and the entry
  classifies each exit once, after its rebuild succeeded. **What still thrashes (decided, not
  fixed here; the shared-code rate or work-based policy that fixes it is in `deferred-work.md`):** a
  function with a loop or a normal return whose designed exit dominates its entry, so that each call
  pays an entry, a rebuild and a re-execution by the interpreter and never gets discarded, because
  nothing counts; above all a function with a short loop (a trip count of one or a few) that ends at
  a designed exit, whose cost per call is the entry and the rebuild with nothing to amortize it.
  It is measurable per call from the statistics: `exits_expected` against `entries + calls` (every
  designed exit is made by an entry or a compiled call, so the ratio is at most one; a function that
  thrashes is near one), and `functions_discarded` stays zero for it. The limit itself is a
  short-run heuristic that must become a rate before compiled code is shared between executions:
  `planning/template-architecture.md`. Compiled code never boxes.
- *Merges.* Where the ways into a jump target disagree on a slot's kind (a local first
  assigned a float inside a loop, `if (c) { x = 1.5; } else { x = 2; }`) every raw source
  of the function becomes an exit and the analysis is run again, so the function is then
  compiled as it was before. The rule is blunt on purpose (finer demotion is an
  optimisation; this one cannot produce a wrong frame state), and the common shape,
  `acc = 0.0` before the loop, merges equal kinds and stays raw.
- *The conversions.* The engine descriptor supplies `convert` (a raw double into a float
  box taken from the pool, which cannot fail, allocate, collect or poll) and `reverse`
  (the type test: true, with the bits, for exactly a float; false for an integer, even one
  that happens to be integral, and for everything else). `slot_kind` and `variable` never
  report a raw double as a value: a raw local is a `RAW` variable with its bits (the
  abstract frame says its representation, `grcore_frame_slot_tagged`; `GRCORE_Variable`
  and `inspect` carry none, so a debugger-grade rendering of a not-yet-rebuilt float needs a
  change in runtime-core (a pause rebuilds before a debugger reads values, which is why none is
  needed yet), and the frame differential reads representations itself, as the value the slot stands
  for, without converting it). A raw *operand* slot (the `1.5` in `1.5 + h(n)` at a poll
  inside `h`) is handled the same way as a raw local: the abstract frame reports it as `RAW`
  with the double's bits and representation `F64`, and a test checks it. The exact gap is in
  `inspect`: for a `RAW` slot it prints the bits as a decimal integer, not the float, because
  `inspect` is given a slot kind and a word and no representation (runtime-core); a debugger
  shows the number only after a pause has rebuilt the frame).
- *The reservation and the pool.* A reservation cell holds a word and a Tang float is a
  heap object, so a conversion that cannot fail or collect needs the object to exist
  already. The boxes are made ahead of time at the two points where an allocation can be
  answered (entering compiled code from the interpreter, and the push hook, before it
  pushes or reads its arguments), kept as precise roots of the execution (so a collector,
  torture and relocation see and update them), and handed out by `convert`. **When the
  invariant holds:** from the moment an entry or a push has extended the reservation (the
  extension and the top-up of the pool to the new capacity are one step) until that extension
  is given back, the pool holds at least as many boxes as the reservation's capacity, and the
  capacity is at least the sum, over the open compiled frames, of each frame's
  `max_converting` (the most converting locations at any one of its sites, recorded in the
  code's payload): the same worst case the core's rebuild checks up front. A rebuild consumes
  boxes, so right after one the pool may hold fewer than the capacity; that is harmless, because
  a rebuild ends its run's compiled code (an outer run, when the rebuild was a nested entry's,
  needs only its own share, which the nested entry's own extension did not touch) and the
  capacity is given back at the same exit. The next extension tops the pool up again. Between
  runs the capacity is zero and the pool is empty (it is let go with control's return to the host,
  asserted there). Entry extends
  by the entry function's, the push hook by the callee's, the pop hook retracts the
  callee's, the deopt hook retracts what its run's calls opened and the entry gives back
  its own at every exit (a LIFO of the nonzero extensions, with a floor for a nested
  entry). A function that holds no raw float extends by zero and allocates nothing. A
  refusal (the memory budget, the system, the extension) is an exit at the call site (the
  interpreter makes the call) or a function not entered this time; it never leaves a
  frame half pushed. A retract keeps the boxes, so a warm loop of calls allocates nothing;
  a box a rebuild consumed is replaced by the next top-up. The pool is let go when control
  returns to the host, because the heap image of a snapshot requires the destination to
  report as many precise root slots as the source did.
- *What a rebuild does.* It is `grcore_compiled_rebuild`, from the deopt hook, and it is the
  only boxing: each live raw value once, with no allocation and no collection across it.
  An unwind converts no frame (lang-tang opens no scope in compiled code, so no compiled
  frame survives an unwind, and the hook skips them all). A poll with nothing pending, and
  a slow-path poll that continues, convert nothing, and the poll helper still touches no
  guest frame.

*Rejected (the record of this decision, AD-27; `runtime-core`'s
"Representation-tagged frame state" and `runtime-jit`'s "Floats and division across the three
backends" give the other libraries' parts and are not repeated here):*

- *In-word doubles (NaN-boxing, or a double with a low-bit tag).* Tang's value representation does
  not change: every pointer is 48 bits and the heap's codec describes a low-bit tag and mask, not a
  NaN pattern, so packing a double into the value word would change how every value in the library
  is read, tested and traced, for one type, and every `is it a pointer` test in the interpreter, the
  collector and compiled code would grow a case. A float stays a heap object outside compiled code
  and is raw only inside it.
- *Allocation inside compiled code.* An allocating helper for each float operation, or a `convert`
  that allocates from the heap under a no-collection flag, cannot be what a rebuild uses: an
  allocation can still fail (the memory budget, the system), and a rebuild that has written one slot
  must not be able to fail. A GC point inside a straight-line float body would also need a stack map
  with a raw value live across it, which the format does not express. The boxes are made ahead of
  time, where a refusal can be answered (the pool, above).
- *Boxing at every poll.* A poll with nothing pending pays a load, a test and a branch and nothing
  more; boxing the live raw values at each poll is an allocation per iteration of every float loop.
  Conversion happens at a rebuild.
- *A dynamic tag in place of the static kinds* (a branch on every access, and a frame state that
  cannot say what a slot is). A representation as another `GRCORE_SlotKind`, a bit-copying default
  `convert`, a format-version bump, floats kept in vector registers between operations and the
  callee-saved FP registers are `runtime-core`'s and `runtime-jit`'s to record, where they are.

The price of the decision is what "The exits that remain, and why" lists, and "Floats, measured"
measures: a float that has to become a value word is an exit.

**Float and division operations compiled (spec-runtime-float story 7, CAP-5).** `+ - * /`,
unary minus, the six comparisons, `as int` and `as float` over a raw double, over a small
integer or a float read from a value word, and over mixes of them; and integer `/` and `%`
over small integers. The rules:

- *Static kinds decide what is a float; the dynamic type only decides whether a guard holds.*
  An operation is a float operation when an operand is statically raw (a float literal, a cast
  to float, or a result of those). A `+ - * /` or a comparison whose two operands are both `REF`
  stays the small-integer instruction, so `function dot(a, b) { return a * b; }` called with
  floats still exits at the multiply. This is forced by AD-27 and static kinds: a result whose
  kind depended on the run-time types of two unknown operands would need a boxed result
  (an allocation) or a dynamic kind tag (rejected above). The consequence is visible in a
  program's shape: the loops that benefit start from a float literal (`s = 0.0`) or a cast
  (`s = n as float`), not from `s = 0`; a loop that starts `x = 0` and adds `0.5` merges an
  integer and a float at its head and falls to the blunt merge rule, compiled as before story 6.
- *Unboxing is a read, inside compiled code, once per use.* The other operand of a float
  operation is read as a double by its word: tag 1 is a small integer (`SAR 4`, `CONVERT_S`,
  which is `(double)n`), a non-null word with tag 0 whose object kind is `GLTANG_OBJ_FLOAT`
  is a float (a 64-bit load at the value's offset; the load of `kind` is reached only after the
  pointer test), anything else (null, a boolean, a string, a boxed integer, a container, an
  error) is a guard exit. The double lives in a temporary `F64` register for the operation: it
  is not cached across instructions, not stored back to the slot, and never named in a frame
  state. No GC point lies between the type test and the load, so a relocating collector has
  updated the word at the last poll. Nothing is allocated.
- *Every guard of an operation precedes the write of its result.* The result is written to its
  operand slot (`stack[..]` or `stack_f[..]` by its kind) after the operation's last guard, so
  the frame state of every guard exit still names the operands as the interpreter's frame holds
  them (a raw operand is `RAW`/`F64` there and a rebuild boxes it once). A guard is tested before
  the operation's fuel is added, like the integer operations.
- *What the interpreter turns into an error or an allocation is a guard.* A float `/` has the
  guard `y != 0.0` on the converted divisor (either zero; a NaN divisor is not zero, because `NE`
  is true for one); integer `/` and `%` guard a zero divisor, `/` also writes the verifier's pair
  (dividend is not the minimum or divisor is not -1; it cannot fire for a small integer, and is a
  real guard site) and range-guards the quotient (`SMALL_MIN / -1` is 2^59); `%` with a raw operand
  is an unconditional exit (the interpreter reports "not supported"); `as int` of a double is
  `TRUNC_S` behind the exact bounds `d >= -2^59` and `d < 2^59` (a NaN fails both ordered
  comparisons; narrower than the verifier's, so accepted), and everything outside them exits so
  the interpreter reports the error or makes the boxed integer. The verifier's patterns (the
  guard on the very register divided by, in the same block, not assigned in between) are why the
  helpers `guard_nonzero` and `trunc_small` are written as they are; a division the verifier
  refuses is a build error and a test failure, not a quiet decline.
- *Kinds.* `ADD SUB MUL DIV` are `F64` if either operand is, else `REF`; `NEG` keeps its operand's
  kind; the comparisons produce a boolean word; `CAST` to float is `F64` (a raw source, an exit when
  a merge disagreed on a kind) and to int is `REF`; `MOD` with a raw operand and a cast to bool or
  to string are exits. `apply_kinds` and `reads_raw` stay the one place that says so, with their
  fail-safe defaults, and the test hook walks every opcode.
- *No allocation, boxing or collection in compiled code.* A raw float becomes a value word only
  when a frame is rebuilt, from story 6's pool; more sites now have raw slots live, so
  `max_converting` may rise, and the reservation follows it with no code change. A function with
  no float constant, no cast to float, no cast to int and no `/` or `%` is emitted as before; integer `/` and `%` are inline now, and an `as int` of a value word declares the two `F64` temporaries (`need_tmp_f`) even when no slot is raw (the temporaries are declared exactly when some slot is ever raw, or a reachable `CAST` to int exists). A float that reaches a
  consumer that is not an operation (`print`, `return`, a call argument, a store to a global,
  truthiness, `as string`, `%`) is still an exit at that instruction; it only boxes the value for its consumer, so it is flagged as expected and does not count toward the discard limit (the decision and its measured effect are in "Floats, measured"). A comparison of a raw float with null or a string (`x == null`, `x != "a"`, `x < "a"`): the interpreter answers (false, true, or "not supported"), but compiled code compares only numbers, so the unbox guard fails and the exit counts as a guard exit (`JitArith.ComparingARawFloatWithNullOrAString*` asserts it).

*Rejected:* speculating that a `REF`-`REF` site is float-float with no feedback (wrong for every
integer loop; each wrong guess exits the other kind); compiling two variants and choosing by
feedback (a different story); caching an unboxed operand in a raw register (it would need an
invalidation story for every assignment, call and poll, and buys one load); a finer merge
demotion (it would force the float producer into a `REF` slot, an exit at the store every
iteration, which the blunt rule gives for the same shape); compiled truthiness of a raw float and
`as bool` (each would be one `FCMP NE x, 0.0`: an easy later addition, not in CAP-5).

#### The exits that remain, and why

Everything below leaves compiled code at the instruction (a guard exit, or the exit that boxes a raw float for its consumer, with the frame state at that instruction) and lets the interpreter do the operation. Each is
marked **structural** (compiled code cannot allocate, AD-27: the operation's result or its operand has to
be a value word, a heap object or an error, so no later story removes it without changing the design) or
**unimplemented** (a later story could compile it, with the cost and the reason given).

| Exit | Why it leaves | |
| --- | --- | --- |
| A raw float reaching `print`, `return`, a call argument (a guest function or a native), a store to a global, an element or a member | the consumer wants a value word, and making one is an allocation | structural |
| `as string` of anything | the result is a string object | structural |
| An error: float `/` by zero, `as int` of a NaN or of a value outside `[-2^59, 2^59)`, a string, array, map or boolean operand of a float operation, `%` with a raw operand ("not supported") | the interpreter's answer is an error value, an object | structural |
| Two `REF` operands (`a * b` of two parameters) | the kind of the result would depend on the run-time types of two unknown words: a raw result needs one operand statically raw, and otherwise it would have to be a boxed result (an allocation) or carry a dynamic tag (rejected) | structural without type feedback; with feedback, unimplemented (a different story) |
| A float local first assigned inside a loop, or assigned values of different kinds on the two arms of a join | the blunt merge rule: the slot is a value word, so the producer would box at every store | unimplemented (a finer merge demotion was rejected: it would force the float producer into a `REF` slot, an exit at the store every iteration, which the blunt rule gives for the same shape) |
| Truthiness of a raw float (`if (x)`), `as bool` | one `FCMP NE x, 0.0` would do | unimplemented |
| A boxed-integer operand (an integer outside the small range) | the unbox reads a small integer or a float and exits on a boxed integer; a load of its 64 bits would do | unimplemented |
| A comparison of a raw float with `null` or a string | the interpreter answers (false, true, or "not supported"); compiled code compares numbers only, so the unbox guard fails (counted as a guard exit) | unimplemented for `null` (the answer is constant), structural for a string (the error is an object) |

`%` of two small integers, `/` of two small integers, and `+ - * /`, the six comparisons, unary minus and
the two casts over raw operands are compiled and are not in the table.

The first row is the one with a price, and "Floats, measured" measures it: a function that ends in a float
return (or a print of one) exits at that instruction at every call. That exit is designed, does not count
toward the discard limit, and a function with no loop whose every path ends there is declined at the compile.

### Why this and not something else

- **Every guest call pushes the callee's guest frame, and compiled code keeps its
  values in its own native frame, vs a shadow frame the compiled code writes at
  each poll (milestone 1).** The first design wrote the guest frame at every
  poll's slow path and kept it current there; it was right while a compiled
  function called nothing, and it makes a chain impossible: a collection at a poll
  would have to find every compiled frame, and the guest frames of the callers are
  stale. Now the guest frame exists (so the depth, the memory and the budget scopes
  count as the interpreter's do, and a pause finds only interpreter frames, AD-8)
  and is written only when the chain is rebuilt; between, the walk pairs each
  compiled frame with its guest frame, the collector reads the compiled frame
  through its stack map and does not read the stale guest frame.
- **The poll helper no longer makes the guest frame current, and certainly does
  not copy it back.** In milestone 1 no collector saw a compiled frame, so the
  helper wrote the guest frame, polled, and wrote the guest frame back into the
  compiled one. With the frame walked and paired, the copy back puts the stale
  guest references over the ones the collector just updated (planted defect 20,
  caught by the relocation arm only: it is invisible until an object moves). A
  reader who finds a reason for the helper to read a frame should read it as a
  reason that hazard is back.
- **A call through the callee's entry slot, guarded on the callee value, vs a table
  of slots indexed by the callee value.** runtime-jit binds a call to one callee:
  the slot is made for a token and a parameter count, and a call through a code
  pointer must name the same two. A callee that is a run-time value could only be
  dispatched by a table if every function had one token and one count, which would
  make a caller pass the largest. The guest has no closures and declares functions
  by a fixed triple, so the common call (a named function) is known when the caller
  is compiled; the guard makes the one assumption that can change (the variable
  holds something else: a declaration that was skipped) an exit and not a wrong
  call. Calls the scan cannot name are exits, until an optimizing tier has
  feedback.
- **The pending fuel lives in the execution, vs in a register copied at each call.**
  A frame state must be exactly the guest frame, and the vreg for the count already
  lives in a stack slot (there is no register allocator), so an add to memory costs
  one more load of the execution's address, and nothing at a chain exit has to
  carry a count. Summing a straight run's cost into one add (what is done) takes
  most of the cost back. A trailing guest-frame slot for the count was rejected: it
  changes the frame layout, the snapshots and the descriptor.
- **The entry poll belongs to the callee's activation.** It cannot move into the
  push hook (a poll that paused there would have no way to rebuild the chain) and
  cannot be made by the caller before the push (every handler would see the caller
  as the top frame). One body with a hidden flag is the cheapest way to keep the
  interpreter's "after the poll" entry (a tier-up in that poll is entered, so a
  threshold of 1 still covers the top level, the documented behaviour) and the
  compiled call's "before the poll" one. Entering compiled code before the
  interpreter's entry poll would have stopped a tier-up in that poll from being
  entered.
- **Identities are set when a chain is rebuilt, from the call sites, vs at every
  call.** The interpreter sets a caller's identity at each call and the walk of a
  rebuilt frame reads it; setting it per call would cost on the path this exists to
  make fast, and a rebuild knows every site.
- **No caller is recompiled when its callee tiers up.** The slot makes it
  unnecessary: the callee's code is installed into the slot, by the interpreter's
  tier-up or by the compile-at-call hook, and every caller calls through it.
- **The guest-depth test is the hook's, once, at the interpreter's condition, vs a
  depth count in compiled code.** The same condition gives the same verdict at the
  same call (the guard on the push by `grcore_stack_push` backs it up, so a hook
  that tested one level late would be invisible; one that tests one level early
  makes a compiled call exit one call too soon, which only the number of compiled
  calls shows, and a test counts them, planted defect 17).
- **Tier-up in ACT with entry after the entry poll, vs on-stack replacement.**
  Entering at a function's start needs the state at one place, with an empty stack.
  Replacing a running interpreter frame at a loop's back-edge needs a mapping from
  every loop head's interpreter state to compiled registers and an entry point at
  each. A loop that is hot in a function called once stays in the interpreter for
  that call.
- **A counter on `POLL` vs call counts.** A poll is the interpreter's one common
  place for entry and back-edge, so one counter sees a hot loop that a call count
  would miss. The cost is one load, one increment and one compare per poll when
  tier-up is on.
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
| `fib(15)`, 1,973 calls, and `fib(22)`, 57,313 | **Milestone 1's defect, and its repair (spec-runtime-calls story 8).** With every `CALL` an exit the JIT cost: story 15 measured `fib(15)` 12% slower than interpreted and `fib(22)` 1.2% slower. Compiled calls reverse that; the figures, with their instrument, machine and date, are recorded once in "Calls, measured", below, and are not repeated here |

**Re-measured on the final tree.** The figures in this table are story 15's, from an Intel Core 7 150U with GCC 14, and are not comparable with a run on another machine. The same cases on the EVO-X2 (GCC 16.2.0, 2026-10-11, `b33d54f`, minimum of five runs, "Floats, measured" has the instrument): the 10-million-iteration integer loop 226.6 ms interpreted (22.7 ns an iteration) and 81.5 ms compiled (8.1 ns): **2.8 times faster** (3.2 on the Intel machine), and unchanged by the float work (the story-10 lang-tang on the same libraries reads 81.4 ms too); four statements an iteration 101.6 ms and 43.1 ms, 2.4 times (2.6); a typical small function run once 0.42 us interpreted and 73.9 us with a threshold of one (0.52 and 76.4). The `fib` row is "Calls, measured" and its re-measurement.

The gain is where the supported set is: a hot loop over small integers and booleans, called often enough to be entered, or entered once at a function's start, and calls between declared functions. A loop in a function that is called once stays in the interpreter for that call (no on-stack replacement), and the main program's top level is never counted past its entry poll for the same reason. What a template-heavy program costs: a template call is an exit, so the interpreter runs the template and enters its function 0 compiled only when the threshold covers it; that is the cost the spec recorded as accepted, and "Calls, measured" has the figure of a template called 200 times.

### Calls, measured (spec-runtime-calls, CAP-6, AD-26)

This is the one place the call-heavy figures are recorded. The other libraries point here: `runtime-core`, `runtime-heap` and `runtime-debug` keep their own micro-benchmarks, and `runtime-jit` has no interpreter, so its "Benchmarks" section holds the micro-figures of a compiled call and points here for `fib`.

**The instrument and the machine.** `bench/bench.c` (the `fib-15-*`, `fib-22-*`, `jit-native-call-1M-*`, `run-native-call-1000` and `run-template-call-200` cases; an engine case's unit is one run of a fixed program, and the result of each `fib` run is checked inside the case: 610 and 17,711). Built in the build image (`ghoti-build:gcc16`: GCC 16.2.0, `-O2 -g -std=c17`, release, `JIT=yes`) from lang-tang `209c8fd` (which differs from story 9's last code commit, `2ce7d39`, only in a test assertion, a planted-defect patch, the planted-defect script and documentation), run on the EVO-X2 (AMD Ryzen AI MAX+ 395, Linux 7.2.9) through `tools/evo`, with the box otherwise idle (load 0.24 before the run, 1.21 after). The harness takes the best of 1,000 runs inside a `fib(15)` case and of 100 inside a `fib(22)` case, and the whole set was run five times in turn; the figure is the **minimum of the five**, with the **spread** (maximum less minimum) in brackets. Date: 2026-10-09. To reproduce: `make -C libs/lang-tang build/linux/release/apps/bench/bench PREFIX=<prefix>` and `build/linux/release/apps/bench/bench`, or `--only fib-` for these cases; it prints the calibration first (1.17 ns a step in this run) and, after each `fib` case, the call statistics below.

| Case | Interpreted | Compiled | Compiled, calls off (milestone 1) |
| --- | --- | --- | --- |
| `fib(15)`, 1,973 calls | 147.0 us (10.9) | **132.3 us (9.8)** (re-measured 2026-10-11 on the repaired tree `b33d54f`: 132.2 us (5.8); see "Integer-only functions and the float work") | 171.9 us (10.0) |
| `fib(22)`, 57,313 calls | 4,178.9 us (402.1) | **2,682.8 us (248.3)** (re-measured on `b33d54f`: 2,714 us (148)) | 4,240.6 us (379.9) |
| a million library calls in a loop of one function (`jit-native-call-1M-*`; "off" is natives switched off) | 59.3 ms (6.3) | **41.9 ms (4.2)** | 61.6 ms (5.3) |
| a template called 200 times, three prints each (`run-template-call-200`) | 88.4 us (5.4): a template call is an exit, so the template runs in the interpreter and this is the figure | n/a | n/a |
| a native called 1,000 times (`run-native-call-1000`), interpreter | 60.9 us (6.2) | n/a | n/a |

**What these figures are: a fresh engine for every timed run (the methodology of every figure in this subsection and in "Benchmarks" except the steady-state table below).** `run_source_with` in `bench/bench.c` creates a group, a context, a heap and an execution for each timed iteration, runs the program once, and destroys them; the clock covers `grcore_run` only, so the engine's creation and teardown are outside it, and everything the run does is inside it: the interpreter's first touches of its own pages and tables, and for a compiled case the counting of polls to the threshold, the compile and the entry, with the compiled code then thrown away with the engine. A `fib(15)` row is therefore one call of `fib` that pays all of that; `fib(22)` is one call, a larger share of which is steady running; the library-call row is one function that runs a million-iteration loop once (compiled at its first poll), so the compile is a negligible share of it; the template row is 200 calls in one run, interpreted throughout (a template call is an exit; the default threshold is not reached). Tang's real use is the other one, compile once and call many times (a template or function compiled once and called until it is replaced or the server stops), and nothing in these rows measures that: "Steady state", below, does, and says how far apart the two are.

**Compiled `fib` is faster than interpreted, and the call between compiled functions is not an exit.** `fib(15)` is **10% faster** compiled (132.3 against 147.0 us; the gain, 14.7 us, is larger than the larger spread, 10.9 us) and `fib(22)` **36% faster** (2,682.8 against 4,178.9 us; 1,496 us against a spread of 402). With no call site compiled, which is what milestone 1 did, `fib(15)` is 17% slower than interpreted and `fib(22)` 1.5% slower, within its spread. `fib(15)` pays the compile once, inside the figure. The per-call cost: 4,178.9 us over 57,313 calls is 73 ns a call interpreted and 47 ns compiled, a compiled call being 64% of an interpreted one. The statistics the harness prints after each `fib` case, the same in all five runs: `fib(15)` 1,764 compiled calls and `fib(22)` 57,096 (the first call of each, and the callee entered from the interpreter, are not compiled calls), **call exits 0** in all four classes (remembered, push refused, callee guard, native stack), no compile at a call (the interpreter's tier-up compiled `fib` first), deepest chain 12 and 19, no hook handed an argument it refuses. The success signal's "no exit at any call between compiled functions" is asserted, not only printed: `JitCalls.ALongCompiledRunOfFib...` in `tests/unit/test_jit_calls.cpp` requires `exits(stats)` to be zero at `fib(22)` (`fib(14)` under the heavy instruments), and planted defect 31 (the push hook refuses once the chain is eight deep, which changes no output and only the count) fails it, with the control passing. "Faster compiled than interpreted" is a recorded figure, not an asserted budget; no numeric budget is asserted in a test. The other two halves of the success signal have tests of their own and are not duplicated: the pause fifty compiled frames down that resumes on another thread (`JitCalls.AChainPausedFiftyFramesDownResumesOnAnotherThread`, with the single-thread `...APauseFiftyFramesDown...`, in `test_jit_calls.cpp`) and the frame differential at every poll over the call-heavy corpus (`test_observer.cpp`).

**Against the figures recorded before (story 8, GCC 14, the same machine, five runs).** `fib(15)` interpreted 146.4 us (spread 9.0), compiled 129.9 (10.6), calls off 170.0; `fib(22)` interpreted 4,252 us (462), compiled 2,664 (183), calls off 4,311. Now: 147.0, 132.3, 171.9 and 4,179, 2,683, 4,241. Every difference is inside the spread of the run it is compared with (the largest, `fib(15)` compiled, is 2.4 us against 10.6), so none is a change; the compiler is the one variable that moved (GCC 14 then, GCC 16.2 now), and that compiled `fib(15)` reads 10% faster here against 11% there is the same finding. The 12% and 1.2% of milestone 1 (story 15) are from an Intel Core 7 150U, a different machine: on the EVO-X2 the same behaviour (calls off) is 17% and 1.5% slower than interpreted, so the two machines are not one measurement.

**Re-measured on the final tree (2026-10-11, `b33d54f`, five runs, the same machine, compiler and flags).** Every row of this subsection reads as recorded: `fib(15)` interpreted 141.8 us, compiled 132.2 (spread 5.8), calls off 168.2; `fib(22)` interpreted 4.20 ms, compiled 2.71 (148 us), calls off 4.21; a million library calls 59.1 and 42.7 ms; the steady-state warm calls, compiled `fib(15)` 90.5 us and `fib(22)` 2.64 ms against 149.9 us and 4.25 ms interpreted (1.66 and 1.61 times as fast, where the table recorded 1.63 and 1.68 on a loaded box). The float work briefly made compiled calls slower (`c2ed592`: `fib(22)` compiled 3.32 ms, `fib(15)` 150.5 us, bisected and repaired in `104929a`); the history, with the figures of each tree, is in "Integer-only functions and the float work: a regression found, bisected and repaired", and no figure of the regressed tree is left in this subsection.

**On real arm64 (a Raspberry Pi 4, Cortex-A72, Linux 6.18, GCC 16.2.0 aarch64 `-O2 -g` release in the build image `suite/Containerfile.build.arm64`, through `tools/rpi`; `WITH_DEBUG=no`, because the image has no bison or flex and the generated parser and scanner were carried over; the same commit; 2026-10-09; three runs of the same cases in turn, the minimum and the spread in brackets).** The television's controller, with nothing else of ours running. `fib(15)`: interpreted 949.0 us (76.3), compiled 992.4 (5.8), calls off 1,172.8 (9.4). **Compiled `fib(15)` is not faster on this machine, and the story's own bar fails there.** The bar for this case (compiled faster by more than the larger spread, as for the EVO-X2) is not met: at the minima compiled is 4.6% *slower* (992.4 against 949.0 us), and at the medians (the lowest median of the three runs: 965.1 us interpreted, 995.8 compiled) it is 3.2% slower, so on both measures the sign is against the JIT, and the interpreted case's spread (76.3 us) is larger than the difference (43.4 us) only because one interpreted run was slow. Three runs were taken, not five, so the spread is a poor estimate; the figure was not re-run. Whether the one-time cost inside the compiled figure explains it was not measured in this row, and is measured in "Steady state", below, which says that it does. The calls-off case, milestone 1's behaviour, is 24% slower than interpreted. `fib(22)`, where the compile is a small part: interpreted 27,919 us (153), **compiled 20,496 (260), 27% faster**, calls off 28,060 (44), 0.5% slower than interpreted; 487 ns a call interpreted and 358 ns compiled (73% of an interpreted call). The counters are the EVO-X2's: 1,764 and 57,096 compiled calls, call exits 0, deepest chain 12 and 19. A million library calls: 501.3 ms interpreted (19.2), **360.6 compiled (24.8), 28% faster**, 523.5 with natives off (9.6); story 9 recorded 514, 360 and 512 on the same machine. `run-native-call-1000` 508.8 us (25.7) and `run-template-call-200` 772.3 us (18.5). The success signal ("`fib(22)` faster compiled than interpreted") therefore holds on both machines; the `fib(15)` ratio holds on the EVO-X2 and does not on the Pi.

**Steady state (one execution, compile once, call many times).** *Instrument:* the `steady-*` cases of `bench/bench.c` (commit `79a1568` and the one after it). One execution per timed run. A script calls the function once (the **first call**, which includes the counting to the threshold, the compile and the entry), calls it again a number of times (the warm-up: 20 for `fib(15)`, 2 for `fib(22)`, 400 for the template) and then a counted number of times (200, 20 and 2,000); a host native, `tick`, stamps the clock between the phases, so the **warm-call** figure is the timed phase divided by its calls and covers the calls only (and two native calls of about 60 ns in a phase of at least a millisecond). The public API has no way to call a guest function from the host, so a script loop is the way; each call enters the function from the interpreter's top level, as a host's call would. A first-call case runs a script that stops after the first call. Every case checks its answer (610 or 17,711 times the calls; 13 bytes of template output for each call) and prints the JIT counters. *Machine and flags:* the EVO-X2 (AMD Ryzen AI MAX+ 395) and the Raspberry Pi 4 (Cortex-A72), each through its tool, in its build image, GCC 16.2.0 `-O2 -g` release, `JIT=yes`, five runs of the cases in turn (the Pi built with `WITH_DEBUG=no` as above), 2026-10-09; the figure is the **median** of the five runs' bests, with the **minimum** and the **spread** (maximum less minimum) after it. The fresh-engine `fib` cases were run in the same job for comparison. The EVO-X2 was not idle for this run: its load average was 2.4 before it, against 0.24 before the run of the table above, and the fresh-engine `fib` cases read 4 to 10% slower than in that run (`fib(15)` interpreted 157.8 us at the minimum against 147.0), so the two runs' absolute figures should not be mixed; the comparisons below are within one run. *Command:* `make -C libs/lang-tang build/linux/release/apps/bench/bench PREFIX=<prefix>`, then `bench --only steady-`.

| EVO-X2 | First call, interpreted | First call, compiled | Warm call, interpreted | Warm call, compiled |
| --- | --- | --- | --- | --- |
| `fib(15)` | 167.6 us (min 152.0, spread 24.1) | 150.1 us (139.7, 10.7) | 168.5 us (165.8, 9.8) | **103.3 us (102.2, 2.1)** |
| `fib(22)` | 4,912 us (4,807, 128) | 3,082 us (3,056, 27) | 4,959 us (4,877, 148) | **2,956 us (2,915, 86)** |
| template (one call) | 1.44 us (1.43, 0.04) | 1.52 us (1.51, 0.04) | 441 ns (435, 10) | 452 ns (450, 5) |

| Raspberry Pi 4 | First call, interpreted | First call, compiled | Warm call, interpreted | Warm call, compiled |
| --- | --- | --- | --- | --- |
| `fib(15)` | 941.0 us (min 936.9, spread 8.8) | 998.6 us (987.7, 22.4) | 937.1 us (935.2, 5.8) | **703.6 us (697.1, 19.3)** |
| `fib(22)` | 27,153 us (27,107, 159) | 20,795 us (20,532, 1,663) | 27,307 us (27,176, 372) | **20,650 us (20,194, 1,848)** |
| template | 17.8 us (17.8, 0.9) | 18.5 us (18.0, 0.8) | 3,366 ns (3,269, 144) | 3,329 ns (3,270, 114) |

For comparison, the fresh-engine cases of the same job (medians; minima in brackets): EVO-X2 `fib(15)` 161.5 us interpreted (157.8), 148.8 compiled (139.2), 194.7 calls off (177.7), `fib(22)` 4,776 us (4,671), 3,069 (3,031), 4,906 (4,802); Pi `fib(15)` 983.5 us (937.3), 1,008.5 (990.5), 1,162.6 (1,155.0), `fib(22)` 27,055 us (27,023), 20,602 (20,481), 28,016 (27,724). The counters: warm `fib(15)` compiled 435,604 compiled calls, **call exits 0**, one function compiled, no compile failure, deepest chain 14; `fib(22)` 1,317,960 compiled calls, **call exits 0**, deepest chain 21; the same on both machines.

*What it shows.*
- **Steady state, compiled `fib` is much faster than the fresh-engine figures say, on both machines.** EVO-X2: `fib(15)` takes 38.7% less time (103.3 against 168.5 us, 1.63 times as fast) and `fib(22)` 40.4% less (2,956 against 4,959 us, 1.68 times; 52 ns a call against 87), where the fresh-engine rows said 8% and 36%. Pi: `fib(15)` 24.9% less (703.6 against 937.1 us, 1.33 times) and `fib(22)` 24.4% less (1.32 times), where the fresh rows said +2.5% (slower) and 24%. The spreads are small beside the gains (EVO-X2 `fib(15)`: 2.1 against 65 us; Pi: 19.3 against 233 us), so the story's bar, faster by more than the larger spread, holds for every steady-state row.
- **The one-time cost is what made `fib(15)` look slower on the Pi, and it is measured now.** The first compiled call costs 295 us more than a warm compiled call on the Pi (998.6 less 703.6; 291 at the minima) and 47 us more on the EVO-X2 (150.1 less 103.3; 38 at the minima). That is the interpreted part of the call before the threshold is reached, the compile and the entry, and it is not split further here. The Pi's `fib(15)` saves 233 us a call once compiled, so the first call (which is 57 us slower than an interpreted one, 6.1%, the 3 to 5% of the earlier rows) is repaid by the second. The EVO-X2 saves 65 us a call, and its first call is already 17 us faster than an interpreted one because the one-time cost there is smaller than a single call's saving. So **the Pi's `fib(15)` result was compile cost** (and the tier-up that precedes it), as suspected: it is a property of running one short call in a fresh engine, not of the compiled code.
- **The interpreter shows no warm-up of its own.** The first interpreted call is as fast as a warm one on both machines (EVO-X2 `fib(15)`: 167.6 and 168.5 us at the medians; Pi 941.0 and 937.1), so the difference between first and warm is the JIT's.
- **A template's compiled code does not help, and does not persist as code.** The template's function 0 is counted at its entry poll once per call and crosses the default threshold in the warm-up; the tier-up compiles it (`functions compiled` 1) and the JIT state is per function of the execution, so the code is kept between calls (the interpreter enters it at the next call's entry poll, `src/jit/jit.c` keys a function by program and index). But it is entered 8 times and exits 8 times (`entries` 8, `deopts` 8; the likely cause is the template's `print` statements, a call of an engine builtin being an exit, which was not checked), which is the eight exits after which any function is discarded and never compiled again, so by the timed phase the template is interpreted again: steady-state template calls are 441 ns interpreted and 452 ns "compiled" on the EVO-X2, 3,366 and 3,329 ns on the Pi, equal within the spreads (the difference of 2.5% on the EVO-X2 is larger than the spread and is not explained here). A template that compiled without such an exit would keep its code. Template calls remain exits either way, as the spec decided.

**Library calls (story 9).** A function calling a host function that adds one, a million times in a loop of one function called once (compiled at its first poll by a threshold of one): 59 ns an iteration interpreted, 42 ns compiled (29% faster; the gain, 17.4 ms, is 2.8 times the larger spread), and 62 ns with natives switched off, which is what a compiled function paid before story 9 (the interpreter's cost plus the exit). The compiled run makes 1,000,000 native calls and one member load, and no exit at those sites. Story 9 recorded 62.6 (2.6), 45.9 (0.7) and 64.4 (3.0) ms on this machine, with GCC 16 in the same build image and three repetitions, so the compiler is not the difference; the interpreted and off figures are now 5% lower and the compiled one 9% lower, which is more than that run's spread of the compiled case (0.7) and less than this one's (4.2), so the two are not told apart. What a compiled call still pays is the shared wrapper (a record opened and left in the core, the copy of the callee and arguments, the keyed lookup of the execution and the hand-back of the JIT record's unit): a callgrind profile of the compiled loop put about two thirds of it in the core's `grcore_activation_enter` and `leave`, the depth accounting and the wrapper itself, and the generated code a third.

**The interpreter's native call, with the JIT off (carried item: the reliable figure).** `gltang_vm_call_native` (the wrapper every native call goes through, interpreted or compiled) was measured by instructions, not time, with callgrind on `run-native-call-1000 --smoke` (5 runs of 1,000 calls = 5,000 calls; x86-64, **GCC 14.2.0**, `-O2 -g` release, on the development machine, 2026-10-09, `valgrind --tool=callgrind build/linux/<tree>/apps/bench/bench --only run-native-call-1000 --smoke` and `callgrind_annotate --inclusive=yes`). **On the `JIT=no` build it costs 604 instructions a call**, inclusive of the native itself (3,021,870 for the 5,000), of which the core's record enter is 230 and leave 114; on the JIT build it costs **630** (3,151,870): the 26 extra are the check that lends the JIT records' units back, paid by an interpreted call in a JIT build and not at all by `JIT=no`. The whole `native_call_run` is 7.87 million instructions on `JIT=no` and 8.34 million with the JIT. These replace the 610 and 636 recorded by the review of story 9 (a 1% difference in the same method, a compiler or tree change). **The earlier "about 29% more instructions than before the story" (8.73 against 11.25 million for 35,000 calls) is removed as unreproduced:** the tree before the story was not rebuilt for it, the totals include setup that no longer matches, and the method that produced the two totals is not recorded, so the figure is not quoted. What can be said is the cost as it stands: one record and one budget unit per native call in both tiers, 344 instructions of it the core's enter and leave. **Native depth now limits interpreted programs:** every native call, interpreted or compiled, opens a record that costs a unit of the native-depth budget, so a program that nests natives (a native that re-enters guest code that calls a native) is refused at the same nesting level in both tiers, with the recursion-limit error, where before the interpreter had no such bound.

**Retired code, measured (the claim to test).** Compiled code a discard retires stays on the context's retired list until no JIT record is open, so the claim is that the list is bounded by the functions of the program: each is discarded at most once and never compiled again. Read with `grcore_code_retired_peak` on the EVO-X2 (GCC 14, story 8's build; the figures in this paragraph are from the EVO-X2; `make test` runs the same test with 40 functions, 12 under torture or a moving stack, and the 1,000 are `GLTANG_LONG_RUN_FUNCTIONS=1000`): a generated program of 1,000 functions, each called from a compiled loop with a guard that fails eight times, all in one run, discards all 1,000 and the peak of the retired list is **0** (a discard is applied after the entry's record is left, so core releases the retired reference at once); 26 ranges are registered at the end (the program's 25 drivers and its top level); and `fib(30)`, 2,692,536 compiled calls and no call exit, retires nothing (peak 0) and keeps its two ranges registered. The bound holds with room (0 against twice the 1,000 discarded), and it holds because of where the discard is made, not because of a bound in core: a discard made while a record is open (a nested activation) is the case below.

**Retired code with a nested record open (story 9): an open risk, owned by core's epoch.** The same thrashing program run so that the discards are made under an open JIT record: a compiled function calls the driver of each group through a native that re-enters guest code (the `reenter` test native), so each function is discarded at its eighth failed guard while the compiled caller's record is open, and core cannot free what it retires until that record is left. The retired peak is exactly twice the discards, at every size measured (x86-64, GCC 14, `GLTANG_LONG_RUN_FUNCTIONS` = 1, 5, 40, 200, 1,000, `testNative_calls`): **2, 10, 80, 400, 2,000** for 1, 5, 40, 200 and 1,000 discarded, with 3, 3, 3, 7 and 27 ranges registered at the end. A discard retires two references, the registry's range and the slot's code, and both wait. So the result that the list is bounded by twice the functions discarded (story 8's claim, which the test asserts) **sits on its bound, it does not hold with room**, and the growth is linear in the discards made under one open record, released all at once when the outermost record is left. What bounds it here is that a function is discarded once and never compiled again (`discard` in `jit.c` marks it never-compile), so the number of discards is at most the number of functions; one function cannot be discarded repeatedly to grow the list, and the repeated thing is the count across functions. A thrashing run that stays inside one record for a long time, with a program of many functions, therefore holds code in proportion to its size and nothing frees it, and a bound or an epoch for retired code (free what no open record can reach, not only when none is open) is core's design: it is not done here, and the test would not catch a regression that kept the list at exactly twice. **Decided (Corey, 2026-10-09): accepted and documented**, here and in runtime-core's `design.md`, as an open risk. It should be looked at again when a long-lived record outlives many discards (a host that keeps one nested activation open across a long run of a large, thrashing program); `deferred-work.md` names that condition and what would settle it (an epoch in the core).

**A compiled call site, as emitted** (`fib`, `return fib(n - 1) + fib(n - 2)`, the IR printed with `grjit_function_print` and the code disassembled with `objdump -D -b binary -mi386:x86-64`, from a scratch build with a print statement added):

```
b10:  ...                                   the argument: n - 1, its guards charged before the cost
      v16 = cmp.eq v11, #19                 the callee value (v2, loaded from the globals) is the function value 1
      br_if v16, b11, b12                   a mismatch is b12: a note for the deopt hook, then a guard exit
b11:  v2 = call.slot slot(1) callee=1(v3, #0)
        state      fn=1 off=14 [#1, #15, #5, v7, v0, v6, v6, v6, v6]     while the callee runs
        exit state fn=1 off=14 [#1, #14, #7, v7, v0, v2, v3, v6, v6]     before the call: pc at the CALL, callee and argument pushed
```

The state is the guest frame: the function, `pc` after the `CALL` (15) and `sp` 5 as constants, the flags register `v7`, the one local `v0`, and the four operand slots as the zero register `v6` (the callee and the argument are popped). Its machine code, past the argument:

```
 6ef  movabs rax, <slot>          ; the callee's entry slot
 6f9  mov    rax, [rax]
 6fc  cmp    rax, 1               ; empty (0): the compile hook; refused (1): exit
 700  jbe    ...
 706  mov    [rbp-0xd0], rax      ; the entry, kept across the hook
 70d  mov    rax, [rbp-0x38]      ; the argument, into the arguments area
 ...
 724  lea    rax, [rip+0x32]      ; the walk start: this frame's base and the return address
 72f  mov    [rcx+0xf0], rbp      ;   stored in the context before the hook (a GC point)
 736  mov    [rcx+0xf8], rax
 73d  mov    rdi, rcx             ; context
 740  mov    esi, 1               ; the callee's token
 745  lea    rdx, [rbp-0xc8]      ; the arguments area (in the stack map)
 74c  mov    ecx, 2               ; argument count: the argument and the flag
 751  call   <push hook>
 762  jne    ...                  ; refused: an exit
 768  mov    rdi, [rbp-0xc8]      ; the arguments, in registers
 76f  mov    rsi, [rbp-0xc0]
 776  mov    r10, [rbp-0x8]       ; the context
 77a  mov    rax, [rbp-0xd0]
 781  call   rax                  ; the internal entry
 783  test   rdx, rdx             ; DEOPTED: the chain was rebuilt, return it
 786  jne    ...
 78c  mov    [rbp-0x30], rax      ; the result
 794  call   <pop hook>
```

### The environment contract's cost on the call path (2026-10-15)

Passing the execution as the JIT environment (`f0725b9`) made compiled `fib` 5.8% (`fib(15)`) and 6.8% (`fib(22)`) slower than before it, outside the spread of six alternating runs on the EVO-X2. Counted with callgrind on `fib(22)` (`bench --only fib-22-jit-default --smoke`), the whole process went from 70.44M instructions to 77.77M, 128 more for each of 57,096 compiled calls: the call site called a C helper (`gltang_jit_slot_entry_address`) to find its callee's slot, which looked the function record up by token (about 60 with its call); the push hook asked the callee's compiled code for its result count (27); the six-argument hooks and their checks (about 26); and the environment moves in the generated code (about 16). The helper also answered zero for a function with no record, and the call then loaded its entry from address 0.

The repair keeps the contract (the code holds no execution's address; the callee's environment is passed on every call): the slot is found by three loads through the environment (`INSTANCE`, then the execution's JIT state, its table of the program's function records, and the callee's record, whose first word is the slot), after every function of the program was given a slot before any was built (`gltang_jit_prepare_slots`; a slot that cannot be made fails the build, counted in `compile_failures`); the code's result count is checked once, where its slot is bound; and the hooks keep the context's guest stack instead of looking it up through the context's keyed slots at every push and pop. Now 70.53M instructions (+0.1%), and in the six-run comparison compiled `fib(15)` is +2.5% and `fib(22)` -2.8%, both inside the spread. The guard is `JitCalls.ACompiledCallLooksNoFunctionUp...`: `record_lookups` is the same for `fib(8)` and `fib(20)`; planted defect 83 puts a lookup in every push. The record of the comparison, with the other cases and the cost that remains in runtime-jit, is in `_bmad-output/implementation-artifacts/spec-runtime-jit-wasm-ir.md` of the workspace.

### Floats, measured (spec-runtime-float, CAP-6, AD-26)

This is the one place the figures of the compiled float and division operations are recorded. `runtime-jit`'s "Benchmarks" has the micro-figures of the same operations against C and points here for the engine's; `runtime-core` has no case of its own for a rebuild and points here for what a compiled function's exit costs.

**What is measured: the final behavior.** A raw float that reaches a consumer compiled code cannot serve (a `return`, a `print`, a store to a global, a call argument, a cast to string) is boxed by an exit at that instruction. That exit is designed, not a failure: it is flagged where it is emitted, it does not count toward the discard limit of eight (`exits_expected`), and the function stays compiled; real failures (a failed guard, an error case, a merge that leaves a raw source as an exit) count (`exits_counted`) and the eighth discards the function. A function with no loop whose every path ends at such an exit is declined at the compile (`functions_declined_expected`) and runs in the interpreter. The rule and its edge cases are in "The exits that remain, and why". Before this rule every such exit counted, and "The discard limit and the exit that boxes a float" below keeps the measurements that led to it and the before and after.

**The instrument and the machine.** `bench/bench.c` (the `jit-float-*`, `jit-intdiv-*`, `steady-float-*` and `float-*` families). Built in the build image (`ghoti-build:gcc16`: GCC 16.2.0, `-O2 -g -std=c17 -ffp-contract=off`, release, `JIT=yes`), against runtime-core `1bf359e`, runtime-heap `693a0c4`, runtime-jit `6a4d733` and cutil `d440e43`. Three EVO-X2 jobs through `tools/evo`, each on the box otherwise idle: **`float8-two`** (lang-tang `9856492`, library code `c5d1f5d`'s, after the exit rule and its review fixes `4746ce7`, `c5d1f5d`; and, interleaved with it, the library from before the rule, `b606b78`, with the same `bench/bench.c`; the `steady-float-*` and `float-*` rows, 5 runs, 2026-10-12) -- the rows of "Floats, measured" except those of the next sentence; **`float8-am`** (lang-tang `ea3ea8b`, library code `a16f05e`'s, which differs from `c5d1f5d`'s in the review fixes only; the `jit-float-*`, `jit-intdiv-*` rows and the `fib` rows, 5 runs, 2026-10-12), whose loops do not depend on the fixes; **`float8-top`** (lang-tang `2d16d24`, the compile of the top level alone, 5 runs, 2026-10-12). Machine: AMD Ryzen AI MAX+ 395, Linux 7.2.9, 32 hardware threads with SMT active, the cpufreq governor `powersave` under `amd_pstate` (active) with boost on; read in the `float8-two` job, `cpu MHz` 4,979 and `scaling_cur_freq` 5.10 GHz (maximum 5.19 GHz) at the start of its first run. The earlier jobs of this record (`float8-r`, the Benchmarks tables) were run on the same box and the clock was not read in them. Load average: 0.2 before a job's first run and 1.0 to 1.1 during it, which is the harness itself. The engine of every case: unlimited fuel, guest depth 1,024, a 1 MiB native stack, `gltang_heap_options_configure`'s heap (collected mode, auto-collect on). Each case prints the heap's collection and allocation counts for its last run (the whole run of the engine after its creation): the compiled raw-float loop of case 1 reads 0 collections and 4 allocations, the interpreted one 366 and 2,000,003, and the compiled run of case 2 (no raw slot) 366 and 2,000,002, so collections run inside the timed region of the cases that allocate per iteration and none ran in the compiled raw-float loops. Every case checks its answer inside the run (a float by its bits against a C reference of the same loop; an integer by value; the error cases by the kind of the result, because no C reference exists for an error value) and which rule it follows: that it compiled and entered, or was declined (and not entered), and the exact numbers of expected exits, counted exits, discarded functions and declined functions, and that the exits are of exactly one kind (`deopts` is the sum of the classified exits), in `--smoke` too; planted defects 65, 68 to 72, 76 to 78, 81 and 82 show those checks can fail.

*The statistic of this section.* The harness takes the best of 7 repeats inside a run (the `n` of each case is its iterations in a repeat) and the whole set was run **5 times** in turn. Every figure in the tables and the text of this section is the **median of the five runs' bests**, followed in brackets by **(the minimum of the five; the spread, maximum less minimum)**. The subsection on the compiled-call regression and the Benchmarks tables use another statistic, the minimum of five (spread in brackets), and say so. Date 2026-10-12. To reproduce: `make -C libs/lang-tang build/linux/release/apps/bench/bench PREFIX=<prefix>` and `build/linux/release/apps/bench/bench --only <prefix>` for `jit-float-`, `jit-intdiv-`, `steady-float-` (which includes the families `steady-float-return-`, `-print-`, `-store-`, `-arg-`, `-entry-` and `-exit-`), `float-` and `calibration`; the calibration of these runs is 1.2 ns a xorshift step.

**Two methodologies, never merged.** *Fresh engine* (`jit-*`, `float-*` without `steady-`): a group, a context, a heap and an execution are created outside the clock for each timed run, which pays the interpreter's first touches, the counting to the threshold (the compiled side runs at a threshold of one, so the function compiles at its first poll), the compile and the entry, and throws the code away. *Steady state* (`steady-*`): one execution; a script calls the function once (the first call, which includes the compile), more times (the warm-up: 10 calls) and then 40 counted times, and the host native `tick` stamps the clock between the phases; the compiled side is again at a threshold of one. Both are about one function and neither is the other: the ratio quoted for a claim names which. An earlier informal figure of "about 8x" for a float loop (a three-run `tang` timing of one program on another machine, process start-up and the compile included, asserted nowhere) is retired. Single-call stamps (call 1, call 9) include the return of one `tick` native, about 90 ns, so compare a single-call figure only with another single-call figure; the calls-2-to-8 and calls-10-on figures are divided by their calls.

**Cases 1 to 6: the loops.** One call of `f(n)` (n = 1,000,000 fresh; 100,000 for each call of the steady-state cases), JIT off against a threshold of one. "Ratio" is interpreted over compiled, from the medians in the cells.

| Program | Fresh engine: interpreted | compiled | ratio | Steady, warm call: interpreted | compiled | ratio | Steady, first call: interpreted | compiled |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| case 1: `s = 0.0`, `return s` (one exit, at the return) | 86.39 ms (84.56 ms; 2.33 ms) | 10.69 ms (10.68 ms; 22.78 us) | 8.08 | 8.67 ms (8.39 ms; 489.46 us) | 1.07 ms (1.07 ms; 45.63 us) | 8.08 | 8.62 ms (8.58 ms; 306.76 us) | 1.12 ms (1.10 ms; 47.84 us) |
| case 1 control: the same loop, `return s as int` (no exit) | 87.76 ms (87.20 ms; 2.45 ms) | 10.69 ms (10.68 ms; 22.83 us) | 8.21 | 8.79 ms (8.66 ms; 638.67 us) | 1.07 ms (1.06 ms; 27.03 us) | 8.22 | 8.77 ms (8.62 ms; 452.22 us) | 1.11 ms (1.10 ms; 47.17 us) |
| case 2: `s = 0` (the merge rule: no raw slot) | 88.21 ms (87.53 ms; 1.53 ms) | 89.54 ms (88.00 ms; 2.51 ms) | 0.99 | 8.58 ms (8.47 ms; 373.91 us) | 8.66 ms (8.58 ms; 333.90 us) | 0.99 | 8.77 ms (8.70 ms; 230.96 us) | 8.91 ms (8.88 ms; 269.01 us) |
| case 3: `s = s + (i as float) / 3.0` | 117.06 ms (113.91 ms; 10.43 ms) | 11.52 ms (11.50 ms; 35.96 us) | 10.17 | 11.14 ms (10.96 ms; 471.43 us) | 1.15 ms (1.14 ms; 28.09 us) | 9.70 | 11.53 ms (11.37 ms; 282.66 us) | 1.18 ms (1.18 ms; 27.40 us) |
| case 3: `s = s * 0.999 + i` | 88.62 ms (85.85 ms; 4.17 ms) | 10.72 ms (10.70 ms; 40.09 us) | 8.27 | 8.52 ms (8.36 ms; 440.04 us) | 1.07 ms (1.07 ms; 25.35 us) | 7.98 | 8.71 ms (8.65 ms; 286.53 us) | 1.10 ms (1.10 ms; 26.68 us) |
| case 4: `x < y`, `x == y`, `x >= y` over raw operands | 114.17 ms (112.45 ms; 5.99 ms) | 23.14 ms (23.10 ms; 55.67 us) | 4.93 | 11.80 ms (11.42 ms; 859.49 us) | 2.31 ms (2.31 ms; 57.13 us) | 5.11 | 11.73 ms (11.32 ms; 1.02 ms) | 2.38 ms (2.38 ms; 59.60 us) |
| case 5: `s = s + i / 3` | 41.15 ms (40.23 ms; 1.22 ms) | 15.05 ms (15.00 ms; 83.17 us) | 2.73 | 4.17 ms (4.14 ms; 62.14 us) | 1.50 ms (1.50 ms; 22.35 us) | 2.78 | 4.17 ms (4.12 ms; 152.53 us) | 1.54 ms (1.54 ms; 23.45 us) |
| case 5: `s = s + i % 7` | 40.03 ms (39.92 ms; 1.04 ms) | 14.07 ms (14.02 ms; 69.96 us) | 2.84 | 3.99 ms (3.94 ms; 111.05 us) | 1.40 ms (1.40 ms; 21.70 us) | 2.84 | 4.03 ms (4.00 ms; 97.61 us) | 1.45 ms (1.44 ms; 22.53 us) |
| case 5: `s = s + i / ((i % 5) + 1)` (steered divisor) | 56.58 ms (55.97 ms; 1.65 ms) | 20.02 ms (19.97 ms; 96.90 us) | 2.83 | 5.58 ms (5.53 ms; 184.46 us) | 2.00 ms (1.99 ms; 27.46 us) | 2.79 | 5.60 ms (5.53 ms; 111.48 us) | 2.04 ms (2.04 ms; 24.64 us) |
| case 5: `s = s + i % ((i % 5) + 1)` (steered divisor) | 54.46 ms (53.56 ms; 1.42 ms) | 18.87 ms (18.82 ms; 73.91 us) | 2.89 | 5.32 ms (5.23 ms; 216.27 us) | 1.88 ms (1.88 ms; 27.86 us) | 2.83 | 5.32 ms (5.24 ms; 200.60 us) | 1.93 ms (1.92 ms; 28.34 us) |
| case 6: `s = s + (i * 0.5) as int` | 67.34 ms (66.59 ms; 2.41 ms) | 15.76 ms (15.74 ms; 172.44 us) | 4.27 | 6.81 ms (6.64 ms; 309.86 us) | 1.57 ms (1.57 ms; 26.16 us) | 4.33 | 6.81 ms (6.76 ms; 104.46 us) | 1.61 ms (1.61 ms; 29.27 us) |
| case 6: `x = x + (i as float)` | 81.46 ms (81.02 ms; 3.70 ms) | 10.41 ms (10.40 ms; 21.25 us) | 7.83 | 8.01 ms (7.80 ms; 363.69 us) | 1.04 ms (1.03 ms; 23.95 us) | 7.72 | 8.23 ms (8.00 ms; 315.11 us) | 1.07 ms (1.07 ms; 26.86 us) |

What these show:

- **A float loop that starts from `0.0` is compiled 8.08 times as fast with a fresh engine** (case 1: 86.39 ms (84.56 ms; 2.33 ms) against 10.69 ms (10.68 ms; 22.78 us); the gain, 75.70 ms, is larger than the larger spread, 2.33 ms). The compiled run exits once at the `return` (`exits_expected 1`). The whole run's `deopts` is 2: the script's own top level is compiled too and leaves compiled code once, at its first function declaration (`exits_counted 1`), so the function's share is one exit; its rebuild boxes both raw values live at the `return` (`values_boxed 2`: the local `s` and the value being returned, one box for each live raw value, never more). The heap counters are 4 allocations and 0 collections, independent of n.
- **In the steady state the float-returning loops keep their gain in every call**, because the exit at the return no longer discards the function: case 1 is 7.68 times as fast in its first call and 8.08 times in the warm calls (1.07 ms (1.07 ms; 45.63 us) against 8.67 ms (8.39 ms; 489.46 us)); case 3, `/ 3.0`, 9.74 and 9.70; `s * 0.999 + i` 7.89 and 7.98; case 6 `as float` 7.66 and 7.72. The programs that return an integer were never discarded and are as before: the `return s as int` control 7.91 and 8.22, case 4 (the comparisons) 4.92 and 5.11, case 6 `as int` 4.22 and 4.33. Before the exit rule the warm calls of the four float-returning programs were 1.00, 1.02, 1.01 and 1.00: the function was discarded at the eighth return.
- **Case 2, the accumulator that starts from the integer `0`, stays at interpreted speed** (89.54 ms (88.00 ms; 2.51 ms) compiled against 88.21 ms (87.53 ms; 1.53 ms) interpreted, ratio 0.99, inside the spread): the head of the loop merges an integer and a float, the blunt merge rule leaves a raw source as an exit that is a real exit (it counts: `exits_counted` is one for every call), and every iteration allocates a float box (2,000,002 allocations and 366 collections in the compiled run, `values_boxed 0`). The eighth call discards the function (the steady-state warm ratio is 0.99). Starting from `0.0` is the whole difference: the `0` form is 8.38 times as slow compiled. This is the cost of the shape, measured, and not a defect.
- **Mixed integer and float, comparisons and casts, fresh engine** (ratios interpreted over compiled): case 3 10.17 (`/ 3.0`) and 8.27 (`s * 0.999 + i`); case 4, three comparisons an iteration over raw operands, 4.93; case 6, `as float` 7.83 and `as int` of a raw double 4.27.
- **Integer division and modulo are compiled 2.73 to 2.89 times as fast with a fresh engine** (case 5, constant and steered divisors; in the steady state 2.78 to 2.83). The guards (the zero test and the range check on the quotient) never fire in the timed loop: `deopts` is 1 (the top level's) in every compiled run.

**Cases 7 to 9, 11 and the controls: a function that ends in a float exit.** `g()` runs `a = 1.5`, a loop of L additions (L = 0, meaning no loop at all, 1, 100 and 10,000) and then the consumer: a `return a * 2.0`, a `print(a * 2.0)`, a store to a global, or a call of another function with `a * 2.0` as its argument; every one of them is an exit at that instruction (a float that must become a value word). The two controls do the same float work and exit nowhere: **(ii)** `return (a * 2.0) as int`, which holds a raw float (so it extends the reservation and tops up the pool at every entry) and returns an integer, and **(i)** an integer-only `g` with the same loop. All of them are called from a script loop at a threshold of one and stamped as call 1, calls 2 to 8, call 9 and calls 10 on. With L = 0 the function has no loop, so the four consumers are **declined** (`functions_declined_expected 1`): their "compiled" rows run in the interpreter, which is the point.

| Body (loop iterations before the exit) | JIT | call 1 | calls 2-8 (each) | call 9 | calls 10 on (each) |
| --- | --- | --- | --- | --- | --- |
| return, 0 (no loop) | interpreted | 207 ns (202 ns; 8.6 ns) | 124 ns (121 ns; 8.6 ns) | 212 ns (210 ns; 4.5 ns) | 113 ns (112 ns; 6.7 ns) |
| return, 0 (no loop) | compiled | 1.73 us (1.70 us; 49.3 ns) | 132 ns (131 ns; 2.5 ns) | 220 ns (219 ns; 9.1 ns) | 115 ns (115 ns; 1.7 ns) |
| return, 1 | interpreted | 328 ns (324 ns; 7.3 ns) | 199 ns (198 ns; 4.2 ns) | 292 ns (290 ns; 6.8 ns) | 195 ns (190 ns; 10.3 ns) |
| return, 1 | compiled | 29.93 us (29.72 us; 536 ns) | 407 ns (402 ns; 14.3 ns) | 497 ns (491 ns; 14.0 ns) | 380 ns (375 ns; 9.7 ns) |
| return, 100 | interpreted | 5.82 us (5.68 us; 229 ns) | 5.75 us (5.62 us; 304 ns) | 5.84 us (5.71 us; 293 ns) | 5.98 us (5.87 us; 272 ns) |
| return, 100 | compiled | 31.12 us (30.96 us; 683 ns) | 1.35 us (1.33 us; 24.9 ns) | 1.45 us (1.42 us; 39.1 ns) | 1.33 us (1.31 us; 50.2 ns) |
| return, 10,000 | interpreted | 574.18 us (564.57 us; 25.41 us) | 570.46 us (560.42 us; 24.23 us) | 563.21 us (552.33 us; 23.40 us) | 561.31 us (550.32 us; 32.18 us) |
| return, 10,000 | compiled | 122.90 us (122.11 us; 2.83 us) | 93.79 us (92.29 us; 2.69 us) | 93.73 us (92.43 us; 3.13 us) | 93.90 us (92.57 us; 3.37 us) |
| print, 0 (no loop) | interpreted | 391 ns (383 ns; 29.4 ns) | 213 ns (210 ns; 13.0 ns) | 313 ns (309 ns; 12.2 ns) | 196 ns (193 ns; 10.6 ns) |
| print, 0 (no loop) | compiled | 2.07 us (2.05 us; 39.4 ns) | 219 ns (216 ns; 7.5 ns) | 323 ns (317 ns; 9.6 ns) | 198 ns (194 ns; 9.1 ns) |
| print, 1 | interpreted | 514 ns (511 ns; 33.1 ns) | 290 ns (288 ns; 6.2 ns) | 395 ns (390 ns; 7.5 ns) | 282 ns (275 ns; 7.2 ns) |
| print, 1 | compiled | 30.63 us (30.47 us; 579 ns) | 589 ns (584 ns; 7.7 ns) | 704 ns (689 ns; 20.0 ns) | 561 ns (558 ns; 4.1 ns) |
| print, 100 | interpreted | 6.07 us (6.02 us; 205 ns) | 5.97 us (5.94 us; 250 ns) | 6.04 us (6.01 us; 241 ns) | 6.25 us (6.18 us; 247 ns) |
| print, 100 | compiled | 31.42 us (31.36 us; 221 ns) | 1.58 us (1.57 us; 34.5 ns) | 1.69 us (1.67 us; 38.8 ns) | 1.56 us (1.54 us; 48.4 ns) |
| print, 10,000 | interpreted | 568.66 us (564.35 us; 18.03 us) | 566.06 us (562.16 us; 18.14 us) | 570.01 us (567.17 us; 16.21 us) | 564.36 us (549.20 us; 31.97 us) |
| print, 10,000 | compiled | 123.22 us (122.58 us; 2.93 us) | 93.86 us (92.56 us; 3.15 us) | 94.00 us (92.67 us; 3.24 us) | 94.16 us (92.80 us; 3.32 us) |
| global store, 0 (no loop) | interpreted | 220 ns (213 ns; 11.9 ns) | 132 ns (128 ns; 8.1 ns) | 221 ns (206 ns; 19.3 ns) | 120 ns (115 ns; 8.9 ns) |
| global store, 0 (no loop) | compiled | 1.85 us (1.82 us; 35.3 ns) | 137 ns (133 ns; 6.2 ns) | 222 ns (216 ns; 11.8 ns) | 119 ns (117 ns; 4.1 ns) |
| global store, 1 | interpreted | 342 ns (330 ns; 21.2 ns) | 209 ns (205 ns; 12.0 ns) | 304 ns (296 ns; 16.1 ns) | 204 ns (198 ns; 12.6 ns) |
| global store, 1 | compiled | 31.79 us (31.49 us; 345 ns) | 413 ns (411 ns; 6.7 ns) | 509 ns (504 ns; 6.9 ns) | 388 ns (386 ns; 5.2 ns) |
| global store, 100 | interpreted | 5.69 us (5.59 us; 214 ns) | 5.61 us (5.53 us; 195 ns) | 5.69 us (5.59 us; 241 ns) | 5.85 us (5.73 us; 229 ns) |
| global store, 100 | compiled | 32.20 us (31.78 us; 570 ns) | 1.34 us (1.32 us; 18.1 ns) | 1.43 us (1.42 us; 14.8 ns) | 1.32 us (1.31 us; 25.6 ns) |
| call argument, 0 (no loop) | interpreted | 282 ns (277 ns; 9.1 ns) | 184 ns (183 ns; 6.3 ns) | 273 ns (262 ns; 19.5 ns) | 174 ns (172 ns; 3.9 ns) |
| call argument, 0 (no loop) | compiled | 10.10 us (10.03 us; 211 ns) | 238 ns (234 ns; 10.2 ns) | 331 ns (328 ns; 9.0 ns) | 213 ns (211 ns; 7.0 ns) |
| call argument, 1 | interpreted | 402 ns (399 ns; 18.7 ns) | 264 ns (259 ns; 8.9 ns) | 350 ns (345 ns; 13.3 ns) | 260 ns (253 ns; 10.2 ns) |
| call argument, 1 | compiled | 39.89 us (39.78 us; 585 ns) | 543 ns (536 ns; 11.5 ns) | 623 ns (623 ns; 6.7 ns) | 504 ns (501 ns; 11.9 ns) |
| call argument, 100 | interpreted | 5.78 us (5.75 us; 136 ns) | 5.71 us (5.68 us; 85.4 ns) | 5.80 us (5.75 us; 117 ns) | 6.00 us (5.96 us; 126 ns) |
| call argument, 100 | compiled | 40.41 us (39.77 us; 762 ns) | 1.47 us (1.45 us; 27.4 ns) | 1.55 us (1.53 us; 23.7 ns) | 1.44 us (1.42 us; 27.6 ns) |
| control (ii) cast, 0 (no loop) | interpreted | 222 ns (209 ns; 26.0 ns) | 135 ns (134 ns; 3.9 ns) | 227 ns (223 ns; 10.1 ns) | 124 ns (122 ns; 7.7 ns) |
| control (ii) cast, 0 (no loop) | compiled | 12.71 us (12.71 us; 164 ns) | 155 ns (154 ns; 7.8 ns) | 238 ns (235 ns; 8.5 ns) | 130 ns (129 ns; 7.1 ns) |
| control (ii) cast, 1 | interpreted | 340 ns (330 ns; 23.3 ns) | 211 ns (207 ns; 6.4 ns) | 305 ns (302 ns; 6.6 ns) | 206 ns (200 ns; 13.0 ns) |
| control (ii) cast, 1 | compiled | 32.10 us (31.99 us; 322 ns) | 174 ns (172 ns; 7.3 ns) | 261 ns (247 ns; 16.9 ns) | 146 ns (142 ns; 7.6 ns) |
| control (ii) cast, 100 | interpreted | 5.75 us (5.61 us; 304 ns) | 5.68 us (5.53 us; 389 ns) | 5.78 us (5.62 us; 360 ns) | 5.98 us (5.80 us; 285 ns) |
| control (ii) cast, 100 | compiled | 33.06 us (32.87 us; 376 ns) | 1.10 us (1.09 us; 19.0 ns) | 1.19 us (1.17 us; 31.1 ns) | 1.09 us (1.08 us; 22.5 ns) |
| control (ii) cast, 10,000 | interpreted | 561.91 us (550.92 us; 21.32 us) | 559.71 us (549.24 us; 20.71 us) | 561.99 us (552.30 us; 20.24 us) | 560.15 us (549.31 us; 16.18 us) |
| control (ii) cast, 10,000 | compiled | 124.47 us (124.16 us; 824 ns) | 92.39 us (92.13 us; 1.38 us) | 92.45 us (92.38 us; 1.36 us) | 92.57 us (92.28 us; 1.65 us) |
| control (i) integer, 0 (no loop) | interpreted | 176 ns (165 ns; 17.9 ns) | 117 ns (112 ns; 5.7 ns) | 204 ns (201 ns; 7.4 ns) | 100 ns (98.7 ns; 4.0 ns) |
| control (i) integer, 0 (no loop) | compiled | 13.98 us (13.95 us; 108 ns) | 150 ns (149 ns; 5.0 ns) | 233 ns (227 ns; 13.0 ns) | 126 ns (121 ns; 6.6 ns) |
| control (i) integer, 1 | interpreted | 257 ns (253 ns; 17.4 ns) | 171 ns (167 ns; 9.7 ns) | 259 ns (248 ns; 17.5 ns) | 155 ns (152 ns; 12.3 ns) |
| control (i) integer, 1 | compiled | 37.05 us (36.50 us; 902 ns) | 168 ns (168 ns; 5.0 ns) | 258 ns (251 ns; 11.8 ns) | 144 ns (142 ns; 5.6 ns) |
| control (i) integer, 100 | interpreted | 3.15 us (3.10 us; 166 ns) | 3.08 us (3.04 us; 162 ns) | 3.20 us (3.14 us; 177 ns) | 3.11 us (3.05 us; 111 ns) |
| control (i) integer, 100 | compiled | 38.13 us (37.98 us; 297 ns) | 1.30 us (1.29 us; 19.2 ns) | 1.38 us (1.37 us; 19.7 ns) | 1.28 us (1.27 us; 21.0 ns) |
| control (i) integer, 10,000 | interpreted | 298.78 us (278.14 us; 23.76 us) | 301.04 us (284.07 us; 20.32 us) | 300.71 us (292.01 us; 12.08 us) | 292.23 us (281.46 us; 22.66 us) |
| control (i) integer, 10,000 | compiled | 149.62 us (149.50 us; 902 ns) | 111.96 us (111.92 us; 1.52 us) | 112.03 us (111.82 us; 1.40 us) | 112.17 us (111.94 us; 1.65 us) |

The same functions with a fresh engine, one call in a run (the compile, or the decline, the entry and the exit are all in it; the ratio is interpreted over compiled, so a value below 1 is a loss):

| Body | Fresh engine, one call: interpreted | compiled | ratio |
| --- | --- | --- | --- |
| return, 0 (no loop) | 286 ns (272 ns; 121 ns) | 10.13 us (10.06 us; 144 ns) | 0.03 |
| return, 1 | 391 ns (383 ns; 12.3 ns) | 38.56 us (38.43 us; 915 ns) | 0.01 |
| return, 100 | 5.79 us (5.70 us; 216 ns) | 40.38 us (39.56 us; 1.39 us) | 0.14 |
| return, 10,000 | 562.01 us (556.26 us; 27.82 us) | 131.40 us (130.43 us; 5.10 us) | 4.28 |
| print, 0 (no loop) | 453 ns (449 ns; 10.9 ns) | 10.95 us (10.86 us; 218 ns) | 0.04 |
| print, 1 | 571 ns (562 ns; 16.1 ns) | 39.75 us (39.46 us; 1.07 us) | 0.01 |
| print, 100 | 6.01 us (5.94 us; 251 ns) | 40.59 us (40.09 us; 1.41 us) | 0.15 |
| print, 10,000 | 559.86 us (553.00 us; 23.39 us) | 131.83 us (131.33 us; 4.94 us) | 4.25 |
| global store, 0 (no loop) | 304 ns (302 ns; 13.9 ns) | 10.63 us (10.50 us; 278 ns) | 0.03 |
| global store, 1 | 415 ns (407 ns; 17.1 ns) | 40.73 us (40.55 us; 1.07 us) | 0.01 |
| global store, 100 | 5.77 us (5.74 us; 134 ns) | 41.58 us (41.01 us; 1.74 us) | 0.14 |
| call argument, 0 (no loop) | 365 ns (362 ns; 10.3 ns) | 19.04 us (19.03 us; 414 ns) | 0.02 |
| call argument, 1 | 480 ns (474 ns; 21.9 ns) | 48.80 us (48.41 us; 1.28 us) | 0.01 |
| call argument, 100 | 5.86 us (5.82 us; 132 ns) | 49.81 us (49.10 us; 1.73 us) | 0.12 |
| control (ii) cast, 0 (no loop) | 299 ns (297 ns; 12.9 ns) | 21.73 us (21.47 us; 665 ns) | 0.01 |
| control (ii) cast, 1 | 410 ns (410 ns; 11.6 ns) | 41.17 us (40.51 us; 1.54 us) | 0.01 |
| control (ii) cast, 100 | 5.79 us (5.71 us; 157 ns) | 42.03 us (41.32 us; 1.90 us) | 0.14 |
| control (ii) cast, 10,000 | 562.20 us (554.49 us; 22.63 us) | 133.55 us (132.75 us; 5.15 us) | 4.21 |
| control (i) integer, 0 (no loop) | 251 ns (248 ns; 7.2 ns) | 23.10 us (22.75 us; 668 ns) | 0.01 |
| control (i) integer, 1 | 333 ns (328 ns; 15.1 ns) | 46.41 us (45.51 us; 1.75 us) | 0.01 |
| control (i) integer, 100 | 3.22 us (3.11 us; 141 ns) | 47.58 us (46.68 us; 1.96 us) | 0.07 |
| control (i) integer, 10,000 | 291.55 us (276.95 us; 19.12 us) | 158.02 us (157.30 us; 5.56 us) | 1.85 |

What these show:

- **(a) The entry cost of a float-carrying function that does not exit is not told apart from zero.** Control (ii) against control (i), calls 2 to 8 at L = 1: 174 ns against 168 ns each, a difference of 5.9 ns (spreads 7.3 ns and 5.0 ns); calls 10 on, 146 ns against 144 ns. Within one execution the pool is topped up once: `box_pool_made` is 2 for the whole run of nine calls, and the entry that follows finds it full and pays one comparison (a retract keeps the boxes). Whether a compiled control beats the interpreter depends on the body: at L = 1, calls 10 on, (ii) is 146 ns compiled against 206 ns interpreted and (i) 144 ns against 155 ns; at L = 100 they are 1.10 us and 1.30 us against 5.68 us and 3.08 us.
- **(b) The exit that boxes a float costs about 234 ns on every call of a function that returns a float.** Calls 10 on at L = 1, the consumer against control (ii), which does the same float work without the exit: `return` 380 ns against 146 ns, a difference of **234 ns**; `print` 415 ns; global store 242 ns; call argument 358 ns (the print and the call argument include the work of the print and of the call itself, which the interpreter does after the exit). For the return that is the exit, the rebuild of the two live raw values (the local `a` and the value being boxed; `values_boxed` is two for every call) and the interpreter finishing the instruction. It is paid on every call of a function that returns a float, forever, because the function stays compiled; for a body of 1 iteration the compiled call (380 ns) is 186 ns slower than the interpreter's (195 ns), and for a body of 100 iterations it is 4.50 times faster.
- **The pool dropped at every host return is paid again at the next entry.** A fuel-sliced run of 4,000 calls of control (ii) returns to the host 4,000 times with a slice of 30 units of fuel (about one call), 444 with 300 and 45 with 3,000 (the pauses of every run, accumulated); each host return releases the pool and the next compiled entry makes it again: `box_pool_made` is 8,000, 890 and 92 for the three runs, **2.0 boxes for each host return** in all three. The figure per host return (the elapsed time divided by the returns, so it includes a call and the polls in between), interpreted then compiled, with (minimum; spread): slice 30, 288 ns (284 ns; 7.9 ns) against 346 ns (344 ns; 19.4 ns), **57.9 ns more**; slice 300, 1.62 us (1.59 us; 120 ns) against 1.81 us (1.80 us; 81.7 ns), **196 ns more**; slice 3,000, 15.07 us (14.87 us; 1.20 us) against 16.72 us (16.48 us; 780 ns), **1.65 us more**. Every difference is larger than the spreads of both of its figures here, so the pool's re-making (with the call and the poll it comes with) is measurable at every slice; the figure grows with the slice, presumably because a longer slice holds more calls, each of them compiled and entered (not separated), and the 3,000 run also contains the compile of a function (spread over 45 host returns about 0.7 us a return). The figures do not separate the pool's re-making from the rest of what a compiled call costs over an interpreted one in that loop, so they bound it above: up to 57.9 ns per host return at one call a return. The two trees ran interleaved in one job, so the before and after differences are comparable (their interpreted figures at this slice are 296 ns and 288 ns; the earlier job, on another day, read 235 and 292 ns for the same two situations): 70.3 ns before and 57.9 ns after at a slice of 30.
- **(c) A loop-free function that ends at the exit is declined, and a per-request shape no longer pays for it.** With a fresh engine and one call, L = 0: 10.13 us (10.06 us; 144 ns) compiled against 286 ns (272 ns; 121 ns) interpreted, where before the rule it was 19.95 us (19.58 us; 788 ns) (interpreted 277 ns (272 ns; 122 ns) in the same job). Of the remaining 10.13 us, a script that declares the function and never calls it costs 8.53 us (8.46 us; 120 ns) compiled against 159 ns (158 ns; 71.4 ns) interpreted (job `float8-top`, the compile of the top level alone, which the decline does not avoid). In the steady state the loop-free consumers' calls cost what the interpreter's do: calls 2 to 8 132 ns against 124 ns for the return (before the rule 493 ns, interpreted 128 ns); call 1 1.73 us against 207 ns (before 10.87 us, interpreted 206 ns). The remaining call-1 difference, 1.52 us, is attributed to the analysis that declines the function at its first compile attempt: an inference, not measured separately.

**Case 11: the error exits and NaN arithmetic.**

| Function (`g(x)`) | JIT | call 1 | calls 2-8 (each) | call 9 | calls 10 on (each) |
| --- | --- | --- | --- | --- | --- |
| `x / 0.0` | interpreted | 190 ns (184 ns; 8.9 ns) | 126 ns (124 ns; 4.0 ns) | 214 ns (210 ns; 7.8 ns) | 114 ns (112 ns; 3.3 ns) |
| `x / 0.0` | compiled | 1.57 us (1.57 us; 29.8 ns) | 130 ns (130 ns; 3.7 ns) | 219 ns (212 ns; 12.7 ns) | 114 ns (112 ns; 4.5 ns) |
| `x as int`, x = NaN | interpreted | 166 ns (162 ns; 11.1 ns) | 124 ns (123 ns; 5.8 ns) | 210 ns (203 ns; 17.2 ns) | 113 ns (112 ns; 5.7 ns) |
| `x as int`, x = NaN | compiled | 13.52 us (13.34 us; 377 ns) | 436 ns (425 ns; 26.7 ns) | 236 ns (236 ns; 7.8 ns) | 113 ns (113 ns; 4.5 ns) |
| `x % 2.0` | interpreted | 181 ns (178 ns; 8.5 ns) | 121 ns (120 ns; 3.9 ns) | 207 ns (206 ns; 6.8 ns) | 110 ns (108 ns; 3.4 ns) |
| `x % 2.0` | compiled | 1.60 us (1.57 us; 52.2 ns) | 125 ns (123 ns; 6.4 ns) | 216 ns (204 ns; 19.7 ns) | 109 ns (107 ns; 5.3 ns) |
| `x * "a"` | interpreted | 909 ns (899 ns; 23.8 ns) | 128 ns (126 ns; 5.2 ns) | 213 ns (209 ns; 6.9 ns) | 110 ns (104 ns; 11.6 ns) |
| `x * "a"` | compiled | 9.83 us (9.77 us; 265 ns) | 426 ns (420 ns; 28.8 ns) | 234 ns (230 ns; 4.8 ns) | 112 ns (111 ns; 4.5 ns) |
| control: NaN arithmetic, `y = x * 1.0; z = y - y`, x = inf | interpreted | 260 ns (256 ns; 23.6 ns) | 182 ns (179 ns; 13.3 ns) | 284 ns (273 ns; 19.2 ns) | 175 ns (171 ns; 10.6 ns) |
| control: NaN arithmetic, `y = x * 1.0; z = y - y`, x = inf | compiled | 21.12 us (20.87 us; 591 ns) | 165 ns (162 ns; 7.7 ns) | 244 ns (233 ns; 23.0 ns) | 136 ns (133 ns; 9.4 ns) |
| control: `(0.0 / x) as int`, x = 2.0 | interpreted | 208 ns (202 ns; 16.5 ns) | 140 ns (138 ns; 4.2 ns) | 236 ns (228 ns; 14.6 ns) | 126 ns (124 ns; 7.0 ns) |
| control: `(0.0 / x) as int`, x = 2.0 | compiled | 16.95 us (16.79 us; 460 ns) | 159 ns (155 ns; 8.7 ns) | 242 ns (238 ns; 11.4 ns) | 132 ns (130 ns; 7.2 ns) |

- **Two of the four error forms are declined and two count.** `x / 0.0` and `x % 2.0` are loop-free functions whose every path ends at an instruction that reads a raw operand, which is an exit that only boxes it, so the rule declines them (`functions_declined_expected 1`; their compiled rows are the interpreter's: call 1 1.57 us and 1.60 us). That is not the recognition of an always-erroring operation, which the rule does not do: another function that always errors would be compiled. `x as int` of a NaN and `x * "a"` end at a real failure (a failed range guard, a failed unbox guard of a string): each call's exit counts, the eighth call discards the function, and the calls after are the interpreter's; calls 2 to 8 cost 312 ns and 298 ns more than the interpreter's and the one-time compile 13.36 us and 8.92 us.
- **NaN arithmetic that does not exit is not an exit**: the control (`y = x * 1.0; z = y - y` with x = infinity, `z != z`) has `deopts 1` (the top level's) and `functions_discarded 0`, and its calls stay compiled (136 ns against 175 ns interpreted), and so has the zero-result control (`(0.0 / x) as int`).

### The discard limit and the exit that boxes a float: the measurements, the decision and its effect

*The question the first measurements asked:* should a function whose every path ends in a float exit be compiled at all, and what does it cost to enter one that always exits? Measured on the library from before the exit rule (`b606b78`, tree B of job `float8-two`, run in the same job and with the same harness as the tree after the rule; the statistic of this section), every such exit counted toward the discard limit, so the eighth call of a function that returned a float discarded it:

| Consumer at the exit | One-time loss, body with no loop | Interpreter, per iteration | Compiled, per iteration | Break-even body (iterations) |
| --- | --- | --- | --- | --- |
| `return` of a float | 13.23 us | 55.3 ns | 9.6 ns | 36 |
| `print` of a float | 13.78 us | 57.8 ns | 10.6 ns | 36 |
| store to a global | 14.28 us | 55.8 ns | 9.7 ns | 39 |
| call argument | 22.70 us | 56.2 ns | 9.6 ns | 61 |
| `x / 0.0` (an error) | 15.32 us | (as the return) | (as the return) | 42 if a loop of the return's kind came before the error |
| `x as int`, x = NaN (an error) | 15.40 us | (as the return) | (as the return) | 42 if a loop of the return's kind came before the error |
| `x % 2.0` (an error) | 11.30 us | (as the return) | (as the return) | 31 if a loop of the return's kind came before the error |
| `x * "a"` (an error) | 11.01 us | (as the return) | (as the return) | 30 if a loop of the return's kind came before the error |

Before the rule, a loop-free body ending in a returned float lost 13.23 us once per execution: call 1 cost 10.67 us more than the interpreter's (the compile, the entry and the first exit) and each of the seven calls after it 366 ns more (that is 10.67 us + 7 x 366 ns); the print lost 13.78 us, the global store 14.28 us, the call argument 22.70 us (two functions compile). It repaid itself from the break-even bodies of the table, and after the eighth exit the function was the interpreter's, so a float-returning loop ran at interpreted speed in the steady state. A per-request shape paid the loss again in every request, because the counter is per execution. The error exits lost 11.01 us to 15.40 us and had no loop to repay it.

**The decision (2026-10-12, taken by Corey after these measurements): an exit whose only reason is boxing a raw float for its consumer does not count toward the discard limit, and a function with no loop whose every path ends at such an exit is declined at the compile.** Real failures still count. The rate-based limit that shared code needs is a separate requirement (`planning/template-architecture.md`) and is not part of this decision.

**The cost of the decision, plainly.** The rule turns a bounded loss into an unbounded per-call one for a float-returning function with a short loop, and for any function whose designed exit dominates its entry: it stays compiled, pays an entry, a rebuild and the interpreter's re-execution of the exit instruction on every call, and nothing ever discards it. Before the rule the eighth exit bounded that loss. Corey accepted the cost on 2026-10-12; the fix, a work-based or rate-based discard policy, is the shared-code requirement and is recorded in `deferred-work.md`. It is measurable from the statistics: `exits_expected` against `entries + calls` is at most one, and a function that thrashes is near one with `functions_discarded` zero. Measured here (EVO-X2, job `float8-two`, calls 10 on, each; interpreted and compiled, before the rule and after it):

| Body (iterations) | Interpreted, before | Compiled, before the rule | Interpreted, after | Compiled, after the rule | After less interpreted (after tree) |
| --- | --- | --- | --- | --- | --- |
| 0 (no loop; declined after the rule) | 116 ns (115 ns; 2.7 ns) | 115 ns (114 ns; 5.0 ns) | 113 ns (112 ns; 6.7 ns) | 115 ns (115 ns; 1.7 ns) | 2.6 ns |
| 1 | 194 ns (190 ns; 14.5 ns) | 193 ns (191 ns; 7.4 ns) | 195 ns (190 ns; 10.3 ns) | 380 ns (375 ns; 9.7 ns) | 186 ns |
| 2 | 259 ns (251 ns; 10.5 ns) | 254 ns (252 ns; 9.1 ns) | 256 ns (255 ns; 6.3 ns) | 391 ns (388 ns; 14.4 ns) | 136 ns |
| 3 | 313 ns (309 ns; 20.2 ns) | 314 ns (306 ns; 12.4 ns) | 322 ns (317 ns; 7.9 ns) | 402 ns (399 ns; 5.4 ns) | 80.1 ns |
| 5 | 424 ns (417 ns; 14.7 ns) | 422 ns (412 ns; 18.7 ns) | 436 ns (429 ns; 8.1 ns) | 421 ns (418 ns; 13.0 ns) | -15.5 ns |
| 10 | 700 ns (683 ns; 34.1 ns) | 700 ns (688 ns; 23.1 ns) | 718 ns (715 ns; 15.2 ns) | 484 ns (482 ns; 11.3 ns) | -234 ns |
| 100 | 5.89 us (5.75 us; 172 ns) | 5.76 us (5.72 us; 190 ns) | 5.98 us (5.87 us; 272 ns) | 1.33 us (1.31 us; 50.2 ns) | -4.65 us |

After the rule the compiled call of a float-returning function is slower than the interpreter's for bodies of 1, 2, 3 iterations by more than the larger spread, and faster from 5 iterations on (the smallest body measured at which compiled wins by more than the larger spread; the bodies 1, 2, 3, 5 and 10 were measured on this machine, and the crossover lies between the last body that loses and the first that wins). Before the rule the same function was bounded: eight such calls, and then the interpreter's speed. The per-call loss grows with the work the exit throws away and falls as the body grows; the largest measured is 186 ns a call.

*Effect, measured: one job, two trees, the same harness.* The tables compare the library from before the rule (B) with the library after it (A), both built against the same dependencies and run interleaved (B then A for each selection of each run) in job `float8-two`, five runs, 2026-10-12, with this tree's `bench/bench.c` (B built with the exit statistics left out, so its rows run unchecked); the harness is therefore the same on both sides, and each cell carries the interpreted figure of its own tree and job, because the two trees' interpreted figures are not controlled for drift between the jobs of different days (the interpreted figure of the same case has moved by 5 to 25% between jobs of different days, for example 235 to 292 ns at a slice of one call) but are for the two trees of this job. The `jit-*` rows of the programs table are from the earlier job `float8-am` (tree `ea3ea8b`).

| Float-returning function (compiled; the interpreted figure of the same tree and job beside it) | Before the rule (library `b606b78`) | After (library `a16f05e`/`c5d1f5d`, tree `9856492`) |
| --- | --- | --- |
| case 1 loop, steady-state first call | 1.10 ms (1.10 ms; 21.75 us); interpreted 8.60 ms | 1.12 ms (1.10 ms; 47.84 us); interpreted 8.62 ms |
| case 1 loop, steady-state warm call | 8.62 ms (8.49 ms; 175.36 us); interpreted 8.63 ms | 1.07 ms (1.07 ms; 45.63 us); interpreted 8.67 ms |
| case 3 `/ 3.0`, warm call | 11.09 ms (10.96 ms; 275.00 us); interpreted 11.31 ms | 1.15 ms (1.14 ms; 28.09 us); interpreted 11.14 ms |
| case 6 `as float`, warm call | 8.00 ms (7.89 ms; 270.11 us); interpreted 7.99 ms | 1.04 ms (1.03 ms; 23.95 us); interpreted 8.01 ms |
| body of no loop, call 1 | 10.87 us (10.75 us; 226 ns); interpreted 206 ns | 1.73 us (1.70 us; 49.3 ns); interpreted 207 ns |
| body of no loop, calls 2 to 8, each | 493 ns (479 ns; 22.2 ns); interpreted 128 ns | 132 ns (131 ns; 2.5 ns); interpreted 124 ns |
| body of no loop, calls 10 on, each | 115 ns (114 ns; 5.0 ns); interpreted 116 ns | 115 ns (115 ns; 1.7 ns); interpreted 113 ns |
| body of no loop, fresh engine, one call | 19.95 us (19.58 us; 788 ns); interpreted 277 ns | 10.13 us (10.06 us; 144 ns); interpreted 286 ns |
| body of 1 iteration, calls 10 on, each | 193 ns (191 ns; 7.4 ns); interpreted 194 ns | 380 ns (375 ns; 9.7 ns); interpreted 195 ns |
| body of 2 iterations, calls 10 on, each | 254 ns (252 ns; 9.1 ns); interpreted 259 ns | 391 ns (388 ns; 14.4 ns); interpreted 256 ns |
| body of 3 iterations, calls 10 on, each | 314 ns (306 ns; 12.4 ns); interpreted 313 ns | 402 ns (399 ns; 5.4 ns); interpreted 322 ns |
| body of 5 iterations, calls 10 on, each | 422 ns (412 ns; 18.7 ns); interpreted 424 ns | 421 ns (418 ns; 13.0 ns); interpreted 436 ns |
| body of 10 iterations, calls 10 on, each | 700 ns (688 ns; 23.1 ns); interpreted 700 ns | 484 ns (482 ns; 11.3 ns); interpreted 718 ns |
| body of 100 iterations, calls 2 to 8, each | 1.46 us (1.45 us; 23.9 ns); interpreted 5.66 us | 1.35 us (1.33 us; 24.9 ns); interpreted 5.75 us |
| body of 100 iterations, calls 10 on, each | 5.76 us (5.72 us; 190 ns); interpreted 5.89 us | 1.33 us (1.31 us; 50.2 ns); interpreted 5.98 us |
| body of 10,000 iterations, calls 10 on, each | 561.04 us (555.71 us; 17.29 us); interpreted 560.88 us | 93.90 us (92.57 us; 3.37 us); interpreted 561.31 us |
| call argument, body of 100, calls 10 on, each | 6.07 us (5.94 us; 227 ns); interpreted 6.08 us | 1.44 us (1.42 us; 27.6 ns); interpreted 6.00 us |
| `x / 0.0`, call 1 (a loop-free error function) | 12.85 us (12.80 us; 122 ns); interpreted 185 ns | 1.57 us (1.57 us; 29.8 ns); interpreted 190 ns |
| compiled caller of a declined loop-free helper, calls 10 on, each | 1.53 us (1.48 us; 76.2 ns); interpreted 1.34 us | 1.56 us (1.53 us; 61.0 ns); interpreted 1.37 us |

The float-returning loops are compiled in every call (a warm call of case 1 is 8.08 times as fast as the interpreter's where it was 1.00); the loop-free consumers cost what the interpreter's do and no longer pay the compile, **a saving of about 11.65 us for the return** (the before loss, 13.23 us, less the decline's remaining cost) per execution on the EVO-X2; and the exit is paid on every call instead of eight times: about 234 ns for a `return` (two live raw values), 415 ns for a `print`, 242 ns for a store and 358 ns for a call argument, over the same function without the exit. The compiled caller of a declined helper (`float-caller-declined`: a compiled loop of ten iterations calling a loop-free float helper) leaves compiled code at its first call site on every call, as a remembered exit that counts nowhere: its call costs 1.56 us against the interpreter's 1.37 us after the rule and 1.53 us against 1.34 us before it (there the helper was compiled, its exits counted, and after eight of them the helper was discarded and became the same remembered exit): the rule changes nothing about the end state of that shape, and the end state is a loss per call of the caller.

*What remains, and what to measure later.* The discard limit counts failures per execution (eight, lifetime of the execution). That is a short-run heuristic: once compiled code outlives an execution and is shared (`planning/template-architecture.md`, "the discard policy is a rate, not a lifetime count"), a lifetime count would eventually discard every function that fails rarely, and nothing would re-promote it; the limit must become a rate (failures per entry over a window, or a counter that decays on successful runs) before code is shared. The exit that boxes a float stays out of that rate by design, but it is now paid on every call of a float-returning function, about 234 ns for the rebuild of two live raw values at a `return` on this machine (more raw values live at the exit cost more: the rebuild boxes one per live raw value). In a shared-code world measure: the per-call exit cost against the number of live raw values at the exit (here two); the ratio `exits_expected` / (`entries` + `calls`) per function, which decides whether the work-based or rate-based discard of the thrashing short-loop shape pays; the pool's re-making at every host return for code shared between requests (up to 57.9 ns per host return at one call a return); and the failure rate of real exits (guard failures) per entry, which is what the shared-code limit will count.

### Integer-only functions and the float work: a regression found, bisected and repaired

The float work added bookkeeping to the compiled-call hooks, and a claim that integer-only functions are emitted as before was true for a function without calls and false for calls. This section records what the benchmark found, the repair (lang-tang `104929a`) and the figures before and after. *Statistic of this section:* the **minimum of five runs**, with the spread (maximum less minimum) in brackets; EVO-X2, GCC 16.2.0 `-O2 -g`, release, 2026-10-11.

| Case (minimum of five runs, spread in brackets) | Recorded 2026-10-09 (`209c8fd`, "Calls, measured") | the regressed tree (`c2ed592`) | the repaired tree (`b33d54f`, job `float8-r`) |
| --- | --- | --- | --- |
| `fib(15)` interpreted | 147.0 us (10.9) | 145.71 us (11.06 us) | 141.78 us (7.68 us) |
| `fib(15)` compiled | 132.3 us (9.8) | 150.51 us (6.48 us) | 132.17 us (5.76 us) |
| `fib(15)`, calls off | 171.9 us (10.0) | 169.41 us (10.33 us) | 168.21 us (10.09 us) |
| `fib(22)` interpreted | 4,178.9 us (402.1) | 4.20 ms (104.98 us) | 4.20 ms (263.77 us) |
| `fib(22)` compiled | 2,682.8 us (248.3) | 3.32 ms (146.20 us) | 2.71 ms (148.27 us) |
| `fib(22)`, calls off | 4,240.6 us (379.9) | 4.22 ms (186.52 us) | 4.21 ms (327.91 us) |
| a million library calls, interpreted | not recorded in this form | 60.10 ms (2.62 ms) | 59.13 ms (3.27 ms) |
| the same, compiled | not recorded in this form | 42.42 ms (2.01 ms) | 42.74 ms (2.08 ms) |
| `fib(15)` warm call, compiled (steady state) | not recorded in this form | 113.01 us (4.90 us) | 90.50 us (5.65 us) |
| `fib(22)` warm call, compiled (steady state) | not recorded in this form | 3.28 ms (184.40 us) | 2.64 ms (150.98 us) |
| 10-million-iteration integer loop, compiled | not recorded in this form | 81.39 ms (3.70 ms) | 81.46 ms (3.03 ms) |

- **History.** On the tree of `c2ed592` compiled calls were slower than they had been: `fib(22)` compiled 3.32 ms (146.20 us) against the recorded 2,682.8 us (+23%) and `fib(15)` compiled 150.51 us (6.48 us) against the recorded 132.3 us (+14%), and `fib(15)` with a fresh engine was 3% *slower* than interpreted. The integer loops and the interpreter did not move. `209c8fd`, built against the same libraries as every row of the bisect, reads 2.69 ms (199.28 us) and 130.63 us (5.82 us), so the libraries are acquitted. The bisect over lang-tang's history, each commit built against those same libraries (no confound from a library change):

| lang-tang commit (every row built against the same libraries: runtime-core `08deb9e`, runtime-heap `693a0c4`, runtime-jit `d768c4d`, cutil `d440e43`) | `fib(15)` compiled | `fib(22)` compiled |
| --- | --- | --- |
| `f79b9f8` (the first commit of the float work: the compile flags) | 129.05 us (8.56 us) | 2.71 ms (63.01 us) |
| `2faad56` (floats held raw) | 136.92 us (8.74 us) | 2.91 ms (195.26 us) |
| `948d91f` (the float kinds made fail safe, the pool's life tested) | 150.97 us (6.81 us) | 3.36 ms (151.54 us) |
| `e90c1df` (float and division operations compiled) | 153.22 us (12.58 us) | 3.31 ms (110.01 us) |
| `c2ed592` (the regressed tree; jobs `float8-base` and `float8-bis`) | 150.51 us (6.48 us) | 3.32 ms (146.20 us) |
| `209c8fd` (before the float work, built against the same libraries; job `float8-base`) | 130.63 us (5.82 us) | 2.69 ms (199.28 us) |

  The step is `948d91f`; the +6% at `2faad56` is inside the spread of the run it is compared with (195 us) and is not told apart. Both commits changed the push and pop hooks (`hooks.c`), `boxes.c` and `jit.c`, which a call between compiled functions goes through and a loop does not.
- **Cause and repair** (lang-tang `104929a`): the per-call float bookkeeping of the carried-floats work (a record lookup and a retract in the push and pop hooks from `2faad56`, and the callee frame word added by `948d91f`) ran on every compiled call, for callees that hold no float; the repair keeps it to a callee that holds floats, a push at one load and one compare. The guard test is `JitFloats.ACompiledCallWhoseCalleeHoldsNoFloatDoesNoFloatBookkeepingAtAll`; planted defects 66 and 67 put the bookkeeping back (66 puts the lookup back and fails on `extension_lookups`; 67 alone, in `boxes.c`, records an extension of nothing, which every entry reaches, and fails on `extension_records` with the lookups at 0). A pop gives a record back only if it is its callee's and counts one that is not (`extension_mismatches`, which `--jit-stats` shows). The figures of the repaired tree (job `float8-r`, `b33d54f`) are the last column of the first table; on the final tree (`ea3ea8b`, job `float8-am`, after the exit rule) `fib(15)` compiled is 132.21 us (5.44 us), `fib(22)` compiled 2.71 ms (111.09 us), interpreted 145.30 us (10.70 us) and 4.18 ms (278.89 us) (minimum of five, spread): the exit rule does not move them. The repaired-tree figures: `fib(22)` compiled 2.71 ms (148.27 us), back at the recorded figure within its spread, and `fib(15)` compiled 132.17 us (5.76 us), within the spread of the recorded 132.3 us (9.8); with a fresh engine `fib(15)` compiled is **1.07 times as fast** as interpreted (141.78 us (7.68 us) interpreted) and `fib(22)` 1.55 times, so the calls spec's "compiled `fib` faster than interpreted" holds again.
- **The float-family figures never depended on it**: a function called from the interpreter's top level is entered through the entry, not the push hook. The float-return family, the entry-cost controls and the pool cases were measured on both trees (medians of five runs' bests, as in "Floats, measured") and agree within their spreads: float-return calls 2 to 8 with a body that holds an empty loop (the shape the bench had that day), 519 ns on the regressed tree and 516 ns on the repaired one, and its one-time loss 32.4 us and 32.09 us (the loss of a body with no loop at all, measured later in one job, is smaller: see the table above). The decision about the exit that boxes a float was taken on these figures.
- **The Benchmarks rows that had moved.** Of the four rows re-measured on `c2ed592` as moved, one was the regression (`run-fib-15`, the default-configuration `fib(15)`: 151.7 us on that tree, 133.19 us on the repaired one, against the recorded 131.4). Three were **not** the regression and read the same on the regressed and the repaired tree: the destroy of the 1 MiB template's tree 20.31 ms (recorded 19.4 ms; 20.4 ms regressed), the compile of the 1 MiB template 12.48 ms (recorded 10.6 ms; 12.0 ms regressed), and the paused-and-resumed loop 43.61 us (recorded 47.3 us; 43.5 us regressed; spread 3.9 us): the polling loop is not told apart from its recorded figure (the 3.7 us difference is inside the spread), the other two moved by 5% and 18%, more than their spreads. No source under `src/compile`, `src/parse`, `src/ast` or `src/core` of lang-tang changed between `209c8fd` and now, so the moves are not code on those paths. Candidates not tested: the `-ffp-contract=off` flag the library has been compiled with since `f79b9f8`; a different cpufreq state (the governor is `powersave` and the clock was not recorded for the earlier measurement); other load on the box; a rebuilt dependency (cutil gained a managed pool in that window). They are not attributed to the float work.


**The Raspberry Pi 4 (real arm64).** *Instrument:* the same cases of `bench/bench.c` through `tools/evo --box pi4`, in the arm64 build image (`suite/Containerfile.build.arm64`: GCC 16.2.0 aarch64, `-O2 -g -std=c17 -ffp-contract=off`, release, `JIT=yes`; `SKIP_DEP_CHECK=1 WITH_DEBUG=no` because the image has neither bison nor flex, with the generated parser and scanner carried in a tar placed in the tree for the shipping and removed again), on the Raspberry Pi 4 Model B Rev 1.4 (Cortex-A72, 4 cores), Linux 6.18.50, cpufreq governor `ondemand` (1.8 GHz, the maximum, when read after a run), the television's controller with nothing else of ours running and `mpv` not running, capped at 3 GiB and 3 CPUs. Four jobs, each **3 runs** of its set in turn: `float8-twopi` (the library before the exit rule, `b606b78`, and after it, `9856492`, interleaved in one job with the same `bench/bench.c`: the `steady-float-return-`, `steady-float-entry-` and `steady-float-caller` rows; load 2.0 before and 1.1 during; SMT inactive, no boost, 1.8 GHz = the maximum when read at the start), `float8-pi` (the loops and the `jit-float-`, `jit-intdiv-`, `steady-float-` families, lang-tang `c2ed592`), `float8-rpi` (the call-dependent rows on the tree before the exit rule, `b33d54f`) and `float8-ampi` (the final behavior, lang-tang `ea3ea8b`: `steady-float-` in full, `fib-`, `steady-fib-` and the case 1 fresh loop). The tables below take the loops from `float8-pi`, the return, entry and caller families from `float8-twopi`, and the rest from the last job that measured it; runtime-core `08deb9e`/`1bf359e`, runtime-heap `693a0c4` and cutil `d440e43` in all. The calibration reads 3.34 ns a step. *Statistic:* the **median of the three runs' bests**, followed by **(the minimum of the three; the spread)**; the paragraph on compiled calls uses the **minimum of three** (spread in brackets) and says so. 2026-10-11 and 12. The fresh-engine `float-` families and the pool-per-host-return cases were not run on the Pi. A Pi timing is a real one; no qemu figure is recorded anywhere.

| Program | Fresh engine: interpreted | compiled | ratio | Steady, warm call: interpreted | compiled | ratio | Steady, first call: interpreted | compiled |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| case 1: `s = 0.0`, `return s` (one exit, at the return) | 685.08 ms (666.13 ms; 22.29 ms) | 134.55 ms (133.61 ms; 1.27 ms) | 5.09 | 65.63 ms (65.33 ms; 789.85 us) | 13.41 ms (13.35 ms; 174.50 us) | 4.89 | 66.55 ms (65.98 ms; 574.41 us) | 13.75 ms (13.74 ms; 20.06 us) |
| case 1 control: the same loop, `return s as int` (no exit) | 660.41 ms (659.95 ms; 1.62 ms) | 134.86 ms (134.85 ms; 91.99 us) | 4.90 | 67.99 ms (66.73 ms; 1.56 ms) | 13.34 ms (13.34 ms; 56.38 us) | 5.10 | 66.33 ms (65.78 ms; 633.60 us) | 13.76 ms (13.74 ms; 28.40 us) |
| case 2: `s = 0` (the merge rule: no raw slot) | 662.02 ms (657.82 ms; 5.52 ms) | 683.61 ms (683.59 ms; 3.28 ms) | 0.97 | 66.17 ms (65.47 ms; 1.08 ms) | 66.03 ms (65.71 ms; 417.02 us) | 1.00 | 66.83 ms (66.23 ms; 941.26 us) | 68.75 ms (67.94 ms; 917.48 us) |
| case 3: `s = s + (i as float) / 3.0` | 848.90 ms (848.63 ms; 17.02 ms) | 146.16 ms (146.07 ms; 108.05 us) | 5.81 | 83.57 ms (83.51 ms; 214.94 us) | 14.55 ms (14.54 ms; 57.70 us) | 5.74 | 85.57 ms (84.85 ms; 932.80 us) | 14.85 ms (14.84 ms; 3.19 us) |
| case 3: `s = s * 0.999 + i` | 702.82 ms (699.84 ms; 22.79 ms) | 131.65 ms (131.36 ms; 1.59 ms) | 5.34 | 67.97 ms (67.89 ms; 311.40 us) | 13.14 ms (13.14 ms; 94.02 us) | 5.17 | 69.36 ms (69.29 ms; 225.00 us) | 13.31 ms (13.31 ms; 14.68 us) |
| case 4: `x < y`, `x == y`, `x >= y` over raw operands | 889.46 ms (886.67 ms; 23.53 ms) | 280.25 ms (277.51 ms; 2.89 ms) | 3.17 | 90.17 ms (89.99 ms; 709.08 us) | 27.96 ms (27.82 ms; 148.49 us) | 3.22 | 90.39 ms (90.26 ms; 416.95 us) | 28.54 ms (28.53 ms; 23.65 us) |
| case 5: `s = s + i / 3` | 320.58 ms (309.89 ms; 47.91 ms) | 183.32 ms (183.27 ms; 650.29 us) | 1.75 | 34.67 ms (34.32 ms; 604.99 us) | 18.27 ms (18.25 ms; 40.47 us) | 1.90 | 37.70 ms (36.71 ms; 2.37 ms) | 18.59 ms (18.58 ms; 21.88 us) |
| case 5: `s = s + i % 7` | 308.42 ms (302.32 ms; 6.81 ms) | 177.29 ms (177.02 ms; 284.03 us) | 1.74 | 31.29 ms (31.19 ms; 548.69 us) | 17.66 ms (17.65 ms; 107.29 us) | 1.77 | 32.76 ms (32.44 ms; 1.51 ms) | 17.96 ms (17.96 ms; 10.98 us) |
| case 5: `s = s + i / ((i % 5) + 1)` (steered divisor) | 450.09 ms (449.23 ms; 6.69 ms) | 259.64 ms (259.39 ms; 390.42 us) | 1.73 | 45.34 ms (43.33 ms; 2.08 ms) | 25.83 ms (25.82 ms; 68.88 us) | 1.76 | 46.47 ms (45.94 ms; 1.12 ms) | 26.23 ms (26.23 ms; 20.16 us) |
| case 5: `s = s + i % ((i % 5) + 1)` (steered divisor) | 415.70 ms (407.41 ms; 21.49 ms) | 260.13 ms (259.64 ms; 739.12 us) | 1.60 | 41.30 ms (39.88 ms; 1.99 ms) | 25.92 ms (25.92 ms; 28.01 us) | 1.59 | 42.22 ms (42.14 ms; 660.97 us) | 26.34 ms (26.34 ms; 10.66 us) |
| case 6: `s = s + (i * 0.5) as int` | 560.16 ms (553.89 ms; 15.06 ms) | 186.10 ms (185.97 ms; 259.70 us) | 3.01 | 57.65 ms (55.57 ms; 2.57 ms) | 18.55 ms (18.52 ms; 36.09 us) | 3.11 | 58.22 ms (57.60 ms; 881.94 us) | 18.88 ms (18.88 ms; 3.33 us) |
| case 6: `x = x + (i as float)` | 637.85 ms (636.17 ms; 7.73 ms) | 131.25 ms (130.91 ms; 380.67 us) | 4.86 | 62.72 ms (62.59 ms; 337.62 us) | 13.00 ms (12.85 ms; 269.23 us) | 4.83 | 65.59 ms (65.01 ms; 577.56 us) | 13.17 ms (13.10 ms; 101.11 us) |

- **Fresh engine: a float loop is compiled 5.09 times as fast on the Pi** (case 1; 8.08 on the EVO-X2). **Steady state, final behavior:** the warm calls of case 1 are 4.89 times as fast as the interpreter's (13.41 ms (13.35 ms; 174.50 us) against 65.63 ms (65.33 ms; 789.85 us)), where before the exit rule they were 1.00 (the discard); the `as int` form 5.10. The accumulator that starts from `0` is 0.97 fresh (interpreted over compiled; 0.99 on the EVO-X2): on the Pi the compiled run is about 3% *slower* than the interpreter (683.6 against 657.8 ms at the minima, in the first job, more than either spread); the cause was not examined.
- **Integer division and modulo are 1.60 to 1.75 times as fast** with a fresh engine (2.73 to 2.89 on the EVO-X2), and 3.01 and 4.86 times for the casts of case 6. Why the division gain is smaller here was not measured.

| Body (loop iterations before the exit) | JIT | call 1 | calls 2-8 (each) | call 9 | calls 10 on (each) |
| --- | --- | --- | --- | --- | --- |
| return, 0 (no loop) | interpreted | 1.85 us (1.84 us; 23.2 ns) | 969 ns (949 ns; 23.8 ns) | 1.57 us (1.57 us; 24.4 ns) | 910 ns (883 ns; 39.0 ns) |
| return, 0 (no loop) | compiled | 25.61 us (24.62 us; 1.58 us) | 1.07 us (1.06 us; 27.5 ns) | 1.64 us (1.61 us; 62.1 ns) | 906 ns (904 ns; 4.5 ns) |
| return, 1 | interpreted | 2.98 us (2.97 us; 25.0 ns) | 1.62 us (1.61 us; 34.8 ns) | 2.26 us (2.23 us; 47.1 ns) | 1.59 us (1.58 us; 41.5 ns) |
| return, 1 | compiled | 244.62 us (243.16 us; 1.65 us) | 3.32 us (3.29 us; 58.3 ns) | 3.97 us (3.94 us; 56.5 ns) | 3.11 us (3.08 us; 34.0 ns) |
| return, 100 | interpreted | 47.20 us (47.18 us; 188 ns) | 45.29 us (45.01 us; 295 ns) | 46.10 us (45.54 us; 613 ns) | 46.58 us (46.49 us; 224 ns) |
| return, 100 | compiled | 254.71 us (254.37 us; 1.79 us) | 14.67 us (14.63 us; 58.8 ns) | 15.32 us (15.22 us; 106 ns) | 14.45 us (14.09 us; 360 ns) |
| return, 10,000 | interpreted | 4.46 ms (4.43 ms; 41.87 us) | 4.41 ms (4.40 ms; 50.90 us) | 4.33 ms (4.32 ms; 27.00 us) | 4.40 ms (4.38 ms; 72.16 us) |
| return, 10,000 | compiled | 1.39 ms (1.38 ms; 5.16 us) | 1.14 ms (1.13 ms; 14.16 us) | 1.14 ms (1.14 ms; 5.10 us) | 1.14 ms (1.12 ms; 24.79 us) |
| print, 0 (no loop) | interpreted | 4.39 us (4.36 us; 79.6 ns) | 1.83 us (1.83 us; 35.6 ns) | 2.58 us (2.50 us; 146 ns) | 1.80 us (1.76 us; 68.4 ns) |
| print, 0 (no loop) | compiled | 31.03 us (31.03 us; 685 ns) | 2.02 us (1.97 us; 61.7 ns) | 2.70 us (2.58 us; 147 ns) | 1.83 us (1.81 us; 54.8 ns) |
| print, 1 | interpreted | 5.53 us (5.52 us; 77.3 ns) | 2.57 us (2.52 us; 52.3 ns) | 3.35 us (3.23 us; 193 ns) | 2.56 us (2.54 us; 59.0 ns) |
| print, 1 | compiled | 252.15 us (251.15 us; 1.12 us) | 5.48 us (5.39 us; 118 ns) | 6.13 us (6.04 us; 106 ns) | 5.26 us (5.11 us; 159 ns) |
| print, 100 | interpreted | 51.14 us (51.09 us; 326 ns) | 47.99 us (47.84 us; 287 ns) | 48.71 us (48.66 us; 514 ns) | 50.27 us (50.01 us; 395 ns) |
| print, 100 | compiled | 261.21 us (260.73 us; 491 ns) | 17.41 us (17.23 us; 250 ns) | 18.10 us (17.82 us; 293 ns) | 17.16 us (16.93 us; 282 ns) |
| print, 10,000 | interpreted | 4.50 ms (4.50 ms; 15.81 us) | 4.45 ms (4.45 ms; 34.25 us) | 4.47 ms (4.47 ms; 47.52 us) | 4.46 ms (4.45 ms; 17.66 us) |
| print, 10,000 | compiled | 1.39 ms (1.37 ms; 26.68 us) | 1.15 ms (1.11 ms; 34.67 us) | 1.14 ms (1.10 ms; 44.56 us) | 1.14 ms (1.11 ms; 32.38 us) |
| global store, 0 (no loop) | interpreted | 2.04 us (2.03 us; 71.0 ns) | 1.00 us (995 ns; 33.6 ns) | 1.66 us (1.64 us; 26.9 ns) | 957 ns (951 ns; 7.0 ns) |
| global store, 0 (no loop) | compiled | 27.59 us (27.53 us; 375 ns) | 1.10 us (1.09 us; 7.9 ns) | 1.68 us (1.66 us; 59.2 ns) | 962 ns (962 ns; 3.7 ns) |
| global store, 1 | interpreted | 3.18 us (3.15 us; 68.7 ns) | 1.70 us (1.70 us; 19.5 ns) | 2.37 us (2.34 us; 33.6 ns) | 1.68 us (1.65 us; 39.8 ns) |
| global store, 1 | compiled | 256.11 us (256.03 us; 768 ns) | 3.59 us (3.57 us; 23.5 ns) | 4.21 us (4.19 us; 45.4 ns) | 3.38 us (3.38 us; 9.8 ns) |
| global store, 100 | interpreted | 47.85 us (47.84 us; 282 ns) | 45.35 us (45.28 us; 301 ns) | 45.99 us (45.98 us; 205 ns) | 47.00 us (46.69 us; 358 ns) |
| global store, 100 | compiled | 265.76 us (263.57 us; 4.10 us) | 14.96 us (14.91 us; 71.1 ns) | 15.56 us (15.54 us; 55.2 ns) | 14.77 us (14.76 us; 34.6 ns) |
| call argument, 0 (no loop) | interpreted | 2.42 us (2.42 us; 4.7 ns) | 1.45 us (1.43 us; 62.2 ns) | 2.07 us (2.06 us; 66.5 ns) | 1.37 us (1.37 us; 63.8 ns) |
| call argument, 0 (no loop) | compiled | 119.99 us (119.42 us; 3.22 us) | 2.04 us (2.03 us; 24.4 ns) | 2.46 us (2.43 us; 46.1 ns) | 1.74 us (1.74 us; 9.1 ns) |
| call argument, 1 | interpreted | 3.52 us (3.52 us; 50.0 ns) | 2.11 us (2.10 us; 27.9 ns) | 2.75 us (2.74 us; 8.5 ns) | 2.11 us (2.05 us; 66.4 ns) |
| call argument, 1 | compiled | 354.42 us (353.54 us; 2.25 us) | 4.68 us (4.67 us; 6.7 ns) | 5.09 us (5.07 us; 50.4 ns) | 4.24 us (4.23 us; 15.1 ns) |
| call argument, 100 | interpreted | 48.51 us (48.32 us; 248 ns) | 46.27 us (46.09 us; 855 ns) | 47.06 us (46.97 us; 624 ns) | 48.44 us (48.05 us; 423 ns) |
| call argument, 100 | compiled | 367.88 us (367.08 us; 1.51 us) | 16.05 us (16.00 us; 70.4 ns) | 16.40 us (16.37 us; 53.3 ns) | 15.60 us (15.59 us; 43.0 ns) |
| control (ii) cast, 0 (no loop) | interpreted | 2.04 us (2.02 us; 32.6 ns) | 1.00 us (1.00 us; 48.4 ns) | 1.65 us (1.59 us; 74.0 ns) | 930 ns (927 ns; 35.3 ns) |
| control (ii) cast, 0 (no loop) | compiled | 129.95 us (128.40 us; 2.43 us) | 1.32 us (1.30 us; 68.0 ns) | 1.83 us (1.78 us; 98.3 ns) | 984 ns (981 ns; 57.2 ns) |
| control (ii) cast, 1 | interpreted | 3.21 us (3.15 us; 94.4 ns) | 1.78 us (1.72 us; 89.0 ns) | 2.39 us (2.33 us; 129 ns) | 1.74 us (1.70 us; 112 ns) |
| control (ii) cast, 1 | compiled | 258.90 us (256.91 us; 4.40 us) | 1.56 us (1.53 us; 65.9 ns) | 2.13 us (2.03 us; 170 ns) | 1.22 us (1.18 us; 61.3 ns) |
| control (ii) cast, 100 | interpreted | 47.90 us (47.50 us; 2.04 us) | 45.79 us (45.15 us; 2.18 us) | 46.56 us (46.15 us; 2.07 us) | 47.95 us (47.60 us; 1.94 us) |
| control (ii) cast, 100 | compiled | 270.07 us (267.98 us; 2.91 us) | 12.86 us (12.69 us; 186 ns) | 13.39 us (13.21 us; 195 ns) | 12.54 us (12.37 us; 173 ns) |
| control (ii) cast, 10,000 | interpreted | 4.51 ms (4.47 ms; 183.70 us) | 4.49 ms (4.41 ms; 183.66 us) | 4.53 ms (4.43 ms; 209.12 us) | 4.54 ms (4.41 ms; 163.21 us) |
| control (ii) cast, 10,000 | compiled | 1.40 ms (1.37 ms; 31.88 us) | 1.14 ms (1.11 ms; 31.21 us) | 1.13 ms (1.11 ms; 31.83 us) | 1.14 ms (1.11 ms; 30.67 us) |
| control (i) integer, 0 (no loop) | interpreted | 1.58 us (1.58 us; 136 ns) | 833 ns (833 ns; 62.6 ns) | 1.40 us (1.40 us; 97.7 ns) | 722 ns (715 ns; 66.7 ns) |
| control (i) integer, 0 (no loop) | compiled | 135.16 us (134.09 us; 1.41 us) | 1.29 us (1.28 us; 52.7 ns) | 1.83 us (1.78 us; 96.4 ns) | 968 ns (964 ns; 60.3 ns) |
| control (i) integer, 1 | interpreted | 2.41 us (2.38 us; 89.6 ns) | 1.32 us (1.32 us; 57.6 ns) | 2.03 us (1.97 us; 67.7 ns) | 1.25 us (1.20 us; 43.2 ns) |
| control (i) integer, 1 | compiled | 279.35 us (277.79 us; 3.66 us) | 1.58 us (1.53 us; 56.3 ns) | 2.15 us (2.04 us; 123 ns) | 1.24 us (1.20 us; 40.0 ns) |
| control (i) integer, 100 | interpreted | 26.60 us (26.59 us; 796 ns) | 25.53 us (25.52 us; 942 ns) | 26.16 us (26.10 us; 1.01 us) | 25.46 us (25.45 us; 892 ns) |
| control (i) integer, 100 | compiled | 293.08 us (292.63 us; 2.52 us) | 15.42 us (15.39 us; 52.5 ns) | 16.02 us (15.89 us; 147 ns) | 15.11 us (15.08 us; 68.4 ns) |
| control (i) integer, 10,000 | interpreted | 2.45 ms (2.45 ms; 86.28 us) | 2.47 ms (2.47 ms; 94.50 us) | 2.50 ms (2.50 ms; 94.31 us) | 2.51 ms (2.51 ms; 106.35 us) |
| control (i) integer, 10,000 | compiled | 1.68 ms (1.68 ms; 3.46 us) | 1.40 ms (1.40 ms; 3.86 us) | 1.40 ms (1.40 ms; 684 ns) | 1.40 ms (1.40 ms; 11.15 us) |

| Function (`g(x)`) | JIT | call 1 | calls 2-8 (each) | call 9 | calls 10 on (each) |
| --- | --- | --- | --- | --- | --- |
| `x / 0.0` | interpreted | 1.77 us (1.74 us; 45.4 ns) | 923 ns (914 ns; 9.9 ns) | 1.51 us (1.50 us; 13.2 ns) | 852 ns (844 ns; 32.6 ns) |
| `x / 0.0` | compiled | 25.17 us (24.60 us; 1.37 us) | 1.02 us (1.01 us; 23.8 ns) | 1.57 us (1.57 us; 4.7 ns) | 880 ns (872 ns; 9.8 ns) |
| `x as int`, x = NaN | interpreted | 1.75 us (1.74 us; 20.0 ns) | 888 ns (883 ns; 10.5 ns) | 1.49 us (1.48 us; 21.3 ns) | 853 ns (847 ns; 7.6 ns) |
| `x as int`, x = NaN | compiled | 136.41 us (135.03 us; 1.64 us) | 4.39 us (4.36 us; 107 ns) | 2.21 us (2.21 us; 31.0 ns) | 895 ns (889 ns; 14.2 ns) |
| `x % 2.0` | interpreted | 1.63 us (1.60 us; 47.0 ns) | 895 ns (882 ns; 70.7 ns) | 1.48 us (1.46 us; 41.9 ns) | 810 ns (809 ns; 19.5 ns) |
| `x % 2.0` | compiled | 25.54 us (25.36 us; 950 ns) | 995 ns (978 ns; 20.8 ns) | 1.53 us (1.46 us; 76.1 ns) | 836 ns (831 ns; 7.8 ns) |
| `x * "a"` | interpreted | 12.51 us (12.22 us; 384 ns) | 1.02 us (994 ns; 51.0 ns) | 1.54 us (1.51 us; 30.4 ns) | 861 ns (826 ns; 69.2 ns) |
| `x * "a"` | compiled | 114.39 us (114.17 us; 2.34 us) | 4.37 us (4.34 us; 49.1 ns) | 2.23 us (2.21 us; 35.8 ns) | 853 ns (852 ns; 1.3 ns) |
| control: NaN arithmetic, `y = x * 1.0; z = y - y`, x = inf | interpreted | 2.41 us (2.37 us; 71.7 ns) | 1.34 us (1.31 us; 34.8 ns) | 1.94 us (1.91 us; 51.3 ns) | 1.36 us (1.32 us; 49.0 ns) |
| control: NaN arithmetic, `y = x * 1.0; z = y - y`, x = inf | compiled | 192.06 us (189.40 us; 2.85 us) | 1.44 us (1.41 us; 62.6 ns) | 1.95 us (1.94 us; 136 ns) | 1.11 us (1.09 us; 68.0 ns) |
| control: `(0.0 / x) as int`, x = 2.0 | interpreted | 1.93 us (1.90 us; 60.0 ns) | 1.03 us (1.03 us; 5.0 ns) | 1.60 us (1.60 us; 8.9 ns) | 961 ns (960 ns; 26.1 ns) |
| control: `(0.0 / x) as int`, x = 2.0 | compiled | 159.06 us (158.55 us; 769 ns) | 1.39 us (1.38 us; 20.7 ns) | 1.89 us (1.89 us; 81.2 ns) | 1.06 us (1.05 us; 31.1 ns) |

- **The decision's effect on the Pi** (job `float8-twopi`: the same harness, interleaved, with the interpreted figure of each tree beside the compiled one; the case 1 loop's warm call is from the earlier jobs, `float8-rpi` before (`b33d54f`) and `float8-ampi` after (`ea3ea8b`), which ran on different days: compiled 65.27 ms (65.05 ms; 697.36 us) against 13.41 ms (13.35 ms; 174.50 us) with the interpreted 65.63 ms):

| Float-returning function on the Pi (compiled; the interpreted figure of the same tree and job beside it) | Before the rule (`b606b78`) | After (`9856492`) |
| --- | --- | --- |
| body of no loop, call 1 | 111.33 us (111.05 us; 3.12 us); interpreted 1.90 us | 25.61 us (24.62 us; 1.58 us); interpreted 1.85 us |
| body of no loop, calls 2 to 8, each | 4.84 us (4.75 us; 124 ns); interpreted 969 ns | 1.07 us (1.06 us; 27.5 ns); interpreted 969 ns |
| body of 1 iteration, calls 10 on, each | 1.67 us (1.66 us; 43.4 ns); interpreted 1.67 us | 3.11 us (3.08 us; 34.0 ns); interpreted 1.59 us |
| body of 2 iterations, calls 10 on, each | 2.12 us (2.11 us; 33.1 ns); interpreted 2.12 us | 3.22 us (3.20 us; 30.7 ns); interpreted 2.02 us |
| body of 3 iterations, calls 10 on, each | 2.57 us (2.56 us; 55.2 ns); interpreted 2.62 us | 3.33 us (3.32 us; 30.2 ns); interpreted 2.48 us |
| body of 5 iterations, calls 10 on, each | 3.50 us (3.50 us; 29.7 ns); interpreted 3.51 us | 3.60 us (3.56 us; 44.3 ns); interpreted 3.42 us |
| body of 10 iterations, calls 10 on, each | 5.74 us (5.73 us; 109 ns); interpreted 5.70 us | 4.16 us (4.16 us; 19.2 ns); interpreted 5.61 us |
| body of 100 iterations, calls 10 on, each | 46.47 us (46.09 us; 1.03 us); interpreted 47.43 us | 14.45 us (14.09 us; 360 ns); interpreted 46.58 us |
| body of 10,000 iterations, calls 10 on, each | 4.40 ms (4.38 ms; 113.96 us); interpreted 4.47 ms | 1.14 ms (1.12 ms; 24.79 us); interpreted 4.40 ms |
| compiled caller of a declined loop-free helper, calls 10 on, each | 11.89 us (11.72 us; 447 ns); interpreted 10.50 us | 11.85 us (11.75 us; 164 ns); interpreted 10.36 us |

  **The short-loop cost on the Pi.** After the rule the compiled call of a float-returning function is slower than the interpreter's for bodies of 1, 2, 3, 5 iterations by more than the larger spread, and faster at 10, 100 iterations (bodies 1, 2, 3, 5, 10 and 100 measured; on the EVO-X2 the crossover lies between 3 and 5 iterations, on the Pi between 5 and 10); the largest loss is 1.53 us a call. Before the rule the same function was bounded (eight calls, then the interpreter's speed). The compiled caller of a declined helper costs 11.85 us against 10.36 us interpreted after the rule and 11.89 us against 10.50 us before it: the same end state.

  The exit that boxes a float costs about 1.90 us on every call on the Pi (calls 10 on at L = 1, `return` against control (ii)); `print` 4.04 us, global store 2.16 us, call argument 3.02 us. A loop-free consumer is declined: its compile, 109.43 us over the interpreter's first call before the rule, is replaced by the analysis that declines it (now 23.76 us over the interpreter's first call, about seventeen times the EVO-X2's figure; the cost of the analysis was not measured separately), a saving of about 112.04 us per execution on the Pi where the EVO-X2's, in the same job as its before, is about 11.65 us. Before the rule, the break-even body of the return on the Pi was 52 iterations (job `float8-twopi`, whose loop-free body is smaller than the empty-loop body of the earlier Pi jobs, whose compile cost 240 us and loss 266 us; the print, store and call-argument break-evens were not re-measured with the loop-free body).
- **Entry of a float-carrying function that does not exit** (calls 2 to 8, control (ii) against control (i), L = 1): 1.56 us against 1.58 us, a difference of -23.4 ns (spreads 65.9 ns and 56.3 ns); a compiled call of a body with no loop is not faster than the interpreter's (1.29 us against 833 ns for (i)).
- **Compiled calls on the Pi** (job `float8-ampi`, *minimum of three runs*, spread in brackets; compared with the figures recorded on 2026-10-09 in "Calls, measured", the same statistic; the Pi never ran the regressed tree): `fib(15)` interpreted 967.19 us (17.11 us) (recorded 949.0 us), compiled 1.01 ms (29.88 us) (992.4), calls off 1.17 ms (16.15 us) (1,172.8); `fib(22)` interpreted 27.20 ms (858.04 us) (27,919 us), compiled 20.69 ms (1.64 ms) (20,496), calls off 27.76 ms (661.74 us) (28,060); steady-state warm call, compiled `fib(15)` 697.17 us (25.02 us) (recorded 697.1 as the minimum), `fib(22)` 20.15 ms (1.14 ms) (20,194). The Pi's fresh-engine verdict for `fib(15)` (compiled slower than interpreted, because the compile is inside the figure) is 4.9% slower in this job; it read 4.6% slower on 2026-10-09 (992.4 against 949.0 us) and 7.3% in the job before the exit rule. The compiled figure's differences from the recorded one are of the size of this job's spread for the compiled case (29.88 us), so they are not told apart; in the steady state compiled is 25% faster than interpreted.


### What is not done

- **Calls inside compiled code** were a defect of milestone 1 (Corey,
  2026-10-05: leaving them out was a bug), closed by spec-runtime-calls story 8
  (guest functions) and story 9 (library natives and the loads that produce them);
  "Measured" has the figures. Still exits: a call of a template (the spec's decision:
  a template runs in the interpreter, and its function 0 can be compiled when it
  enters it), of an engine builtin (`random.seeded`, `rng.set_seed`), of a native or
  a function the declaration scan cannot name (a parameter, an array element, a
  global with no declaration), of a resumable native (the test natives only; reached
  through an exit by design, AD-23), of a native with sixteen arguments or more
  (runtime-jit's native call takes the callee and fifteen), a load that does not
  yield a host function or a library, and any call with the wrong number of
  arguments. Tang has no tail call and none is added. Float arithmetic, comparison
  and casts, and integer `/` and `%`, were the same kind of defect until story 7; they
  are compiled now (above). What is left as an exit is listed, with the reason and
  whether it is structural, in "The exits that remain, and why".
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
- **Inline caches**, **inlining of heap operations** (arrays, maps, strings; a
  native is called, not inlined) and boxed-integer arithmetic: exits, and the interpreter does
  them. Float arithmetic, comparison and casts and integer `DIV`/`MOD` over raw or small-integer
  operands are compiled (above); a float that must become a value word, a float's truthiness and
  the error cases are the exits of "The exits that remain, and why". **No rate-based discard
  policy** for shared code: the limit counts failures per execution and must become a rate before compiled code is
  shared (`planning/template-architecture.md`); "Floats, measured" says what to measure then.
- **Hardening** of the generated code (guard pages, randomised layout, constant
  blinding): the pages are never writable and executable at once, and that is all.
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
local `make test` (plus `suite/tools/xarch` and `suite/tools/xwin` for other targets) is the
whole gate, and a claim about a result names where it ran.

## Benchmarks

`bench/bench.c` is the harness (AD-26): a calibration case beside the front
end (parse of a small script, parse of a 1 MiB generated template, destruction
of its tree, compile of both) and the engine (a tight integer loop, a recursive
`fib(15)`, building a string, building an array, and the loop again paused and
resumed every 500 units of fuel). An engine case's unit is one run of a fixed
program, timed from `grcore_run` to its return. `make test` runs it once with a
tiny workload; `make bench` runs it in full. No numeric budget is asserted. The
figures below were re-taken on 2026-10-09, all of the cases in one run of the harness (the
instrument, the machine and the compiler are those of "Calls, measured": the EVO-X2, AMD
Ryzen AI MAX+ 395, GCC 16.2.0 `-O2 -g` release, lang-tang `209c8fd` (which differs from story 9's last code commit, `2ce7d39`, only in a test assertion, a planted-defect patch, the planted-defect script and documentation), through
`tools/evo`; the minimum of five runs of the whole harness, each itself the best of its
repeats), for reading future figures against the calibration beside them. They replace the
first measurement, which was taken on an Intel Core 7 150U with GCC 14 and is in the
history of this file: its `fib(15)` row said 181 us, and said the JIT cost 12%. Both
were true of that machine and of milestone 1's JIT, which left compiled code at every call;
neither is true of this one, and the call-heavy cases are in "Calls, measured".

| Case | Figure |
| --- | --- |
| calibration | 1.17 ns per xorshift step |
| parse of the small script | about 3.4 us |
| parse of a 1 MiB template (about 11,000 rows) | about 41.6 ms |
| destroy of that tree | about 20.3 ms (19.4 ms on 2026-10-09; re-measured 2026-10-11 on `b33d54f`: 20.3 ms, spread 0.2: moved by 5%, more than the spread, not the float work's regression (it reads the same on the regressed and the repaired tree); cause not identified) |
| compile of the small script | about 0.79 us |
| compile of the 1 MiB template | about 12.5 ms (10.6 ms on 2026-10-09; moved by 18%, more than the spread of 0.35 ms; the same on the regressed and the repaired tree, so not the float work's regression; cause not identified) |
| run: 1,000-iteration integer loop | about 40.7 us (41 ns per iteration, about a dozen instructions) |
| run: `fib(15)` (1,973 calls), the default configuration | about 133.2 us (about 68 ns per call; 131.4 us on 2026-10-09, inside the spread; on the way it read 151.0 us on the regressed tree `c2ed592`, repaired in `104929a`). The JIT is built and the threshold is the default, so `fib` tiers up after 200 polls and most of the run is compiled; the interpreted figure, 147.0 us, is in "Calls, measured" |
| run: 200 string appends | about 26.2 us |
| run: 1,000 array element stores | about 58.3 us |
| run: the loop paused and resumed every 500 fuel | about 43.6 us (47.3 us on 2026-10-09, not told apart from it: the 3.7 us difference is inside the spread of 3.9 us; the same on the regressed tree; the pauses add about 6% to the loop beside it, as the first measurement said; spread 3.9 us) |
| run: `use math; s += math.pi` 200 times | about 24.8 us (about 124 ns for a `use`, a member access and an add) |
| run: a native function called 1,000 times | about 60.9 us (about 61 ns a call, the loop included) |
| run: a template call, 200 times (three prints each) | about 88.4 us (about 442 ns a call: the activation, the scope's open and close, the prints and the output string) |
| run: 2,000 swallowed errors, the list full after 1,024 | about 151.0 us (about 76 ns an error; the 976 past the cap are only counted) |
| run: `random.global.next_int % 7` 1,000 times | about 91.0 us (about 91 ns an iteration, a boxed integer or two included) |
| run: four statements to a loop iteration, 1,000 iterations, statement polls off | about 42.1 us (44.3 us on 2026-10-09; within about one spread of 2.3 us) (the `LINE` instruction is executed and does nothing) |
| run: the same, statement polls on with nothing pending | about 92.9 us (about 12 ns a poll on the unarmed fast path) |
| start from scratch: a context made and run until it pauses right after a heavy prologue | about 2.17 ms |
| start from a snapshot: a context made and the snapshot of that pause restored into it | about 0.154 ms (see below) |

Re-measured on 2026-10-11 on `b33d54f` (the same machine, compiler and method; the minimum of five runs of the whole harness): every other row of this table, not named above, is **not told apart** from its figure of 2026-10-09 (the loop 41.1 us against 40.7, the string appends 25.5 against 26.2, the array stores 58.0 against 58.3, `use math` 24.7 against 24.8, the native call 61.2 against 60.9, the template call 86.3 against 88.4, the swallowed errors 149.8 against 151.0, the random loop 87.4 against 91.0, the statement polls on 97.9 against 92.9, the starts 2.11 ms and 0.150 ms against 2.17 and 0.154, the parse rows 3.4 us and 41.3 ms), each inside or within about one spread of the earlier figure.

The host API adds nothing to a run that does not use it: the loop and `fib` cases
are where they were, within the noise of the machine. With the JIT built, the
default threshold costs a run that never tiers up 2 to 5% (see "The baseline JIT",
which also has the cases that measure the compiled loop and the calls).

**The statement-boundary instruction (story 13).** `LINE` is executed even with
statement polls off, so the cases above that run statements are slower than they
were before it existed (figures of that story, on its Intel Core 7 150U with GCC 14, and
not those of the table above; `fib(15)` there is milestone 1's, with every call an exit):
re-measured from the tree before it (83c1fac) and from
this one, alternating, the loop is 64.6 us before and 66.7 us after (+3%),
`fib(15)` 181 us and 192 us (+6%), the template call 107.5 us and 111.1 us (+3%)
and the polling loop 67.6 us and 69.8 us (+3%). The calibration case moved by
under 3% between the same runs, so the figures are good to about that. That is the price of making a statement a place a host may ask to stop at without
patching a shared program; the alternatives are listed under "Statement polls".

**Profiling (story 17, CAP-12).** `profile-loop-10M-off` and `profile-loop-10M-1ms` run the same interpreted ten-million-iteration loop without a profiler and with one whose timer posts every millisecond: 353 ms and 352 ms on that story's machine (234 and 240 ms on the EVO-X2 on 2026-10-09, minimum of five runs, spreads 25 and 23 ms), the difference being inside the run-to-run noise. A sample is a frame walk (about 100 ns at depth one and 30 ns for each frame more, measured in `runtime-core`'s `profile-sample-*` cases), so a 1 kHz timer takes on the order of a ten-thousandth of the run.

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
`Not implemented`, as in ctang. A host can inject arrays and maps (a description: "Host-described values") and
have a native take and return any value ("Handles"), and define its own types
with arithmetic, comparison, attribute, index and slice ("Host-defined value
types"), but not yet iteration, calls, casts, printing or snapshots of them:
ctang could, the rewrite dropped it without a decision, and
`spec-tang-host-extensibility` restores it (ledger D-032 to D-056; D-032 to
D-038 and D-048 to D-056 are fixed). No template arguments, no
`include`, no `try` or `catch` (no new syntax or semantics at all). The execution
differential against ctang is described under "Verification". The ledger is
not closed while D-039 to D-047 are open (see "The ledger, final"). No debugger in the library: the engine registers the frame
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
