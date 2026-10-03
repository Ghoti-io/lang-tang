# Design

**Status:** In progress. Describes what exists: the front end of the Tang
engine (the parser, the scanner and the syntax tree, ported from ctang as this
library's own source), the compiler from that tree to this library's own
bytecode, the switch-dispatched interpreter that runs it on a runtime-core
context and a runtime-heap heap, the interface that parses, compiles and runs a
template or a script, the host API over it (libraries, native functions, the
error list, template calls under budget scopes, a generator per context), the
`tang` command, the divergence ledger, and the oracle that compares this
library with frozen ctang. What is still not here is listed at the end. The
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

`ITER_NEXT` takes two words: the second is the exhausted target. Every
instruction costs one unit of fuel; the operations that do work proportional to
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
returns `GRCORE_ERR_GUEST` with the execution `UNWOUND`. A template call's scope
does not catch it: the vote is the run's own (the scope is not the only voter, so
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
its pause policy.

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
outcomes, and error reporting. The ledger is closed when no row is `open`: with story 10 it is, because
section 13.9 (D-001), the last open row, is a recorded departure - the error is
listed. The execution differential of story 11 may open rows again.

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

The library links `cutil`, `unicode`, `runtime-core` and `runtime-heap` and
nothing else; the `.pc` requires all four, and the manifest lists them, so a
bootstrap builds them first. `readelf -d` on the shared object and on `tang` shows
exactly those, never ctang.

`tools/check-edges.sh` enforces AD-2 over `src/`, `include/`, the generator
inputs, `bench/` and `examples/`, and over the NEEDED list of the shared library
and the `tang` command. The rule is an allowlist: `cutil`, `unicode`,
`runtime-core`, `runtime-heap`, `lang-tang`. Anything else - ctang, the
debugger, the JIT, another engine - is an edge, and so is any include of
`binary.h`. `tests/` is the one place ctang may be included, and a fixture shows
that an include there passes while the same line anywhere else does not.

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
| run: `use math; s += math.pi` 200 times | about 35 us (about 180 ns for a `use`, a member access and an add) |
| run: a native function called 1,000 times | about 59 us (about 59 ns a call, the loop included) |
| run: a template call, 200 times (three prints each) | about 101 us (about 510 ns a call: the activation, the scope's open and close, the prints and the output string) |
| run: 2,000 swallowed errors, the list full after 1,024 | about 208 us (about 100 ns an error; the 976 past the cap are only counted) |
| run: `random.global.next_int % 7` 1,000 times | about 123 us (about 120 ns an iteration, a boxed integer or two included) |

The host API adds nothing to a run that does not use it: the loop and `fib` cases
are where they were, within the noise of the machine.

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
under the three policies (one that finishes, one that never does, one that calls
the first and divides by zero), a seed sequence, and a cap of 64 on the error
list, so the fuzzer reaches resolution, native calls, scopes and the list; six
seed files under `tests/fuzz/corpus/run/` do, and are replayed in `make test`. `make fuzz-replay` feeds
every corpus and seed file once through the same entry points in an ordinary gcc
build and fails on a crash - it is part of `make test`, so a regression a fuzzer
once found is a failing test and not a campaign to repeat. Fifteen seconds of
`fuzz_run` (307,000 executions, 2,940 new units) found nothing on the first
run. No campaign is run here, and no differential fuzzing: that is the story
that compares execution against ctang over generated programs.

## What is not here

No library other than `math` and `random`, and in those no `next_int_range`,
`next_float_range`, `next_gaussian`, `shuffle`, `sample` or `choose`: they stay
`Not implemented`, as in ctang. No arrays or maps injected from the host (scalars,
native functions, templates and libraries only), no template arguments, no
`include`, no `try` or `catch` (no new syntax or semantics at all). The execution
differential against ctang and the closing of the ledger are story 11's: the
oracle stays parse-only, and the ledger has no open row only until that
differential has run. No debugger beyond the frame protocol the engine registers
(the frame walk, scopes and variables read from a paused context, which now name
the right program for every frame). No `simplify`. No JIT, no snapshots. No
parse-time charge to a context's memory (see "The memory-budget contract"). No
CI.

**Story 9's deferred items.** The resolver contract is closed: host callbacks
are opaque, run on the owner thread and cannot re-enter (see "The callback
contract"). Two stay open, with their reasons. The value renderer still holds a
512-byte buffer per recursive frame, so value depth 2,048 needs about 1 MiB of C
stack; it is bounded and a template call does not add to it (a template does not
recurse in C), but a host thread with a small stack is still its risk. The
compiler's operand-ceiling `GLTANG_ERR_LIMIT` arms (16 million constants) have no
test, since reaching them needs a test-only override of `GLTANG_OPERAND_MAX`.
