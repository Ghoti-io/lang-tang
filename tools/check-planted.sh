#!/bin/sh
#
# Prove that each instrument of the verification suite fails on the defect it
# exists to catch (CAP-7).
#
# A gate that has never been seen to fail may be measuring nothing. This builds
# a throwaway copy of the library under build/planted/, applies ONE patch at a
# time from tests/planted/, builds the test that is supposed to notice it and
# requires that test to FAIL, then takes the patch out again, rebuilds, and
# requires the same test to PASS (the control: a test that fails for any reason
# at all would otherwise count as having caught the defect).
#
#   01 missing root           the torture suite        (testExecute_complex, GRHEAP_TORTURE+VERIFY)
#   02 missing gc_store       barrier-verify           (testExecute_complex, GRHEAP_TORTURE+VERIFY)
#   03 order-dependent DECIDE the phase-shuffle run    (testObserver)
#   04 a native that never polls  the native gate      (testNative_gate)
#   05 a wrong frame slot     the frame observer       (testObserver)
#   06 a wrong operator       the oracle differential  (testOracle)
#   07 a silent oracle runner the oracle driver        (testOracle)
#   08 a wrong tag on a compiled ADD   the frame differential, interpreter against JIT (testObserver)
#   09 a skipped fuel charge in compiled code  the fuel-parity test (testJit)
#   (10, a missed write-back at a poll, is retired: the poll helper no longer writes
#    the guest frame at all, and 20 is the opposite mistake)
#   11 a host pointer left in a type's payload with no hook   the address scan of a snapshot (testSnapshot)
#   12 a skipped output-buffer capture   the output-so-far test and the corpus sweep    (testSnapshot)
#   13 the temporaries reported through a copy   the relocation arm   (testExecute_simple, GRHEAP_RELOCATE+TORTURE)
#   14 an array's storage pointer reported through a copy   the relocation arm   (testExecute_simple, GRHEAP_RELOCATE+TORTURE)
#   15 the push hook reads its arguments before the push   the relocation arm   (testJit_calls, GRHEAP_RELOCATE+TORTURE)
#   16 the push hook omits the CALL's fuel   the fuel-parity tests   (testJit_calls)
#   17 the guest-depth test off by one   the recursion-limit test   (testJit_calls)
#   (18, a deopt hook that does not set the callers' identities, is retired: runtime-core's
#    rebuild sets every rebuilt frame's identity, and its test_rebuild guards it)
#   19 the deopt hook ignores a failed rebuild   the injected rebuild failure   (testJit_calls)
#   20 the poll helper copies the guest frame back after a continue   the relocation arm   (testJit_calls, GRHEAP_RELOCATE+TORTURE)
#   21 the slot of an uncompilable callee is not refused   the remembered-exit count   (testJit_calls)
#   22 an exit at a call to a remembered callee counts toward the discard limit   the uncompilable-callee test   (testJit_calls)
#   23 the call passes the entry flag as 1   the entry-form parity test, the poll count   (testJit_calls)
#   24 discard destroys the code directly   the discard test   (testJit_calls)
#   25 a native's activation record is not closed   the native depth the native sees and the loop of library calls   (testNative_calls)
#   26 a native-depth miscount in the units handed back for the JIT records   the depth a native sees and the budget sweep   (testNative_calls)
#   27 a native's arguments are copied before the collection and not pinned   the relocation arm   (testNative_calls, GRHEAP_RELOCATE+TORTURE)
#   28 a stale pointer into the guest stack after a native that re-entered guest code   the nested-activation tests on a moving stack   (testNative_calls)
#   29 an unwind status read as OK   the run that runs out of budget inside a native's answer   (testNative_calls)
#   30 a compiled member load by name is not charged   the differential over a loop of `.name` loads and of a dotted `use` path   (testNative_calls)
#   31 a call made deep in a compiled chain is an exit   the long compiled run of fib, which asserts zero call exits   (testJit_calls)
#   32 a compiled float sum one ulp off   the frame differential's bits channel   (testObserver)
#   33 a compiled subnormal product or quotient flushed to zero   the frame differential's bits channel   (testObserver)
#   34 a compiled zero product that is a NaN   the frame differential's bits channel   (testObserver)
#   (32 to 34 were planted in the interpreter's arithmetic under `exec->jit_threshold` while
#   the compiled float of spec-runtime-float story 7 did not exist, and stood for it; story 7
#   moved each into the compiled emission, so that they are caught by the real thing. The text
#   of a float is six decimals, so for 32 and 33 the text of every slot, the output and the
#   result are unchanged and only the bits differ.)
#   35 the pool of float boxes is not reported as roots   the rebuild under torture and verify   (testJit_floats)
#   36 the reservation is extended one cell short   the pause with k live raw values   (testJit_floats)
#   37 a raw word is in the conversion's root cell across a collection   the rebuild under torture and verify   (testJit_floats)
#   38 reverse has no type test   the checked write-back over a hand-made F64 site   (testJit_floats)
#   39 the pop hook does not give back the callee's extension   the capacity at the caller's polls   (testJit_floats)
#   40 an unwind converts the frames it pops   the unwind with floats live   (testJit_floats)
#   41 a float is boxed at every slow poll   the allocation slope with a handler pending   (testJit_floats)
#   42 a merge of unequal kinds is not noticed   the merge programs against the interpreter   (testJit_floats)
#   43 a float constant is emitted through 32 bits   the pause with live raw values, read back by their bits   (testJit_floats)
#   44 the pool of float boxes is not let go when control returns to the host   the pool test, empty at a pause, and the snapshot that restores   (testJit_floats)
#   45 a refused stack push keeps the reservation it extended   the failed-push test   (testJit_floats)
#   46 the pool's boxes are reported through a copy   the relocation arm   (testJit_floats under GRHEAP_RELOCATE=1 with torture); invisible without relocation
#   47 a failed rebuild leaves raw words in the guest frame   the failed-rebuild test under torture and verify, the failing-rebuild seam   (testJit_floats)
#   48 the float divide guard tests the dividend for zero   the error rows   (testJit_arith)
#   49 an integer operand is converted unsigned   the mixed integer and float rows   (testJit_arith)
#   50 the unbox reads the double field with no kind test   the string, array and boolean operand rows   (testJit_arith)
#   51 >= and <= compiled as the negation of < and >   the NaN comparison rows   (testJit_arith)
#   52 the as-int upper bound is <= 2^59   the cast rows   (testJit_arith)
#   53 the integer quotient is not range-guarded   SMALL_MIN / -1   (testJit_arith)
#   54 a mixed operation writes its result slot before the second operand's guard   the string-operand-after-a-raw-operand row   (testJit_arith)
#   55 a mixed subtraction or division has its operands swapped   the mixed rows   (testJit_arith)
#   56 a float operation adds no fuel   the fuel-parity test   (testJit_arith)
#   57 arithmetic on raw operands is an exit again   the allocation slope and the exit counts   (testJit_arith)
#   58 the integer is converted from its tagged word   the mixed rows   (testJit_arith)
#   59 unary minus on a raw float is 0.0 - x   the -0.0 row   (testJit_arith)
#   60 the unbox does not test that a word is a non-null pointer   the null, boolean and error operand rows (a wild load: killed by SIGSEGV)   (testJit_arith)
#   61 the as-int lower bound is -2^62   the cast rows   (testJit_arith)
#   62 a NaN divisor is read as zero   the division by NaN row   (testJit_arith)
#   63 the % zero-divisor guard is removed (the verifier refuses the function)   the modulo rows   (testJit_arith)
#   64 NEG, the float comparisons, both casts, % and integer / add no fuel   the fuel comparison of expect_same   (testJit_arith)
#   65 the C reference of the benchmark harness's float loop is 1.0 off   the answer check of bench --smoke   (bench)
#   66 every push looks its callee's raw floats up   the float-free call test, counting the lookups across fib(15)   (testJit_floats)
#   67 every extension is recorded, whatever its size (the entry's test of max_converting removed)   the same, counting the records   (testJit_floats)
#   68 the integer reference of the division loop is 1 off   the integer path of the harness's answer check   (bench)
#   69 the steady-state cases expect the answer for n + 1   the steady-state answer check alone   (bench)
#   70 the cast family's expected value is 1 off   the family cases' exact expectation   (bench)
#   71 the compiled rows run at a threshold of zero   a compiled row must have compiled and entered   (bench)
#   72 a function that fails for real is expected never to be discarded   the exact number of functions discarded   (bench)
#   73 the exit that boxes a raw float counts toward the discard limit   the looped float-returner that must stay compiled across fourteen calls   (testJit_arith)
#   74 the static decline fires on a function with a loop   the same, which must be compiled and entered   (testJit_arith)
#   75 the static decline fires on a function with a real exit path beside a float return   the function that must be compiled and entered   (testJit_arith)
#   76 the exits that only box a raw float are expected one more than the calls   exits_expected is the number of calls   (bench)
#   77 a loop-free float-returning function is expected to compile   a function the compile declines is seen declined   (bench)
#   78 the top level's exit is expected not to count   exits_counted is exactly the exits that count   (bench)
#   79 the static decline does not look at a normal return   the function with a float path and a normal integer return, which must compile   (testJit_arith)
#   80 the sites of an exit are taken from the function's record   the discard-while-a-record-is-open test   (testJit_floats)
#   81 the harness's exit-kinds invariant leaves out the remembered call exits   deopts is exactly the sum of the classified exits   (bench)
#   82 the harness expects a declined function's callee never to be entered   a declined function is not entered, its callee is, exactly   (bench)
#   83 every compiled call looks its callee's record up   the lookup-count test: fib to depth 8 and to depth 20 make the same number of lookups   (testJit_calls)
#   84 a container held across a child's allocation (a missing root)   the descriptions read under torture and verify   (testHost_values)
#   85 a raw word in a temporary root cell   the numbers in a map, under torture and verify   (testHost_values)
#   86 a string's encoding tag dropped inside a container   the rendering differentials   (testHost_values)
#   87 a map key's encoding tag dropped   the map-key test   (testHost_values)
#   88 the work of making a value not charged   the fuel a long copy is charged and the budget that stops it   (testHost_values)
#   89 an in-run description allocated from the process and not the context   the memory-budget test of a factory   (testHost_values)
#   90 the materializer's depth ceiling not checked   the over-deep description reached through the test hook   (testHost_values)
#   91 the builder's depth limit not checked   the depth-limit tests   (testDescription)
#   92 the builder's node limit not checked   the node-limit test   (testDescription)
#   93 the builder's byte limit not checked   the byte-limit tests   (testDescription)
#   94 a node given a second parent   the one-parent tests   (testDescription)
#   95 a reach cached and shared by the reaches after it   the fresh-copy tests: two reaches, a reach in a loop, a second execution   (testHost_values)
#   96 an ancestor never compared with the node attached (a cycle can be built)   the cycle tests   (testDescription)
#   97 a container that may hold itself   the self-attach rows of the cycle test   (testDescription)
#   98 the materializer's node limit not checked again   the description past its own node limit, through the test hook   (testHost_values)
#   99 the materializer's byte limit not checked again   the description past its own byte limit, through the test hook   (testHost_values)
#   100 a string not checked for UTF-8 when added   the invalid-text tests   (testDescription)
#   101 the engine's node ceiling not applied (a description is held to its own limit only)   the description that claims no limit, against the lowered ceilings   (testHost_values)
#   102 the engine's byte ceiling not applied   the same   (testHost_values)
#   103 a ref of another description accepted   the foreign-ref test   (testDescription)
#   104 a persistent handle the collector does not see   the persistent handle across collections under torture and verify   (testHandles)
#   105 a handle of another scope accepted   the stale-handle test   (testHandles)
#   106 the handle limit not checked   the scope-limit test   (testHandles)
#   107 a string's segment tags dropped by the reader   the segment test   (testHandles)
#   108 the persistent table not freed at destroy   the context's leak check   (testHandles)
#   109 the persistent limit not checked   the persistent-limit test   (testHandles)
#   110 a function labelled no_gc that collects   the label gate, every no_gc function under torture   (testHost_callbacks)
#   111 the libraries a callback made into values not released at destroy   the count of cutil allocations across a run   (testHandles)
#   112 the native records made at run time not freed   the context's leak check   (testHandles)
#   113 a popped frame's handles kept   the frame test   (testHandles)
#   114 a handle answer read after the scope is closed   the answer tests   (testHandles)
#   115 a container stored without the copy the language makes   the copy test   (testHandles)
#   116 a scope serial reused   the stale-handle test   (testHandles)
#   117 the persistent values reported to the collector through a copy   the persistent handle under relocation torture   (testHandles, GRHEAP_RELOCATE+TORTURE); invisible without relocation
#   (104 to 117 are the handles of spec-tang-host-extensibility story 3, all in src/vm/handle.c and libvalue.c.)
#   118 a template call's own output delivered to the sink   the template output test   (testHost_context)
#   119 the delivered point never advanced   the delivery test   (testHost_context)
#   120 a refused print kept in the buffer   the refusal test   (testHost_context)
#   121 the language layer searched before the program's libraries   the precedence test   (testHost_context)
#   122 the language layer an execution took never released   the count of cutil allocations across a run   (testHost_context)
#   123 the current language layer read at every use   the replacement test   (testHost_context)
#   124 the language layer retained but not sealed   the readable-layer test   (testHost_context)
#   125 one host context for every execution   the two-executions test   (testHost_context)
#   126 render that encodes nothing   the render test   (testHost_context)
#   127 concat that drops the second string's tags   the concat test   (testHost_context)
#   128 a substring one grapheme too long   the substring test   (testHost_context)
#   129 a value factory handed no host context   the factory test   (testHost_context)
#   130 a sink called without the host-function flag   the re-entry test   (testHost_context)
#   131 a restored snapshot delivering its old output again   the snapshot delivery test   (testHost_context)
#   132 a refused print of text kept in the buffer   the template refusal test   (testHost_context)
#   133 the language layer read without its lock   ThreadSanitizer on the threaded layer test   (testHost_context; the TSan arm only: not in the quick set, run by the story's verification job)
#   134 a snapshot that names the language layer as the built-ins   the snapshot-of-a-layer-member test   (testHost_context)
#   135 the language layer taken without a reference   the library-freed-with-the-last-execution test   (testHost_context)
#   136 a print delivered before it is known to be whole   the failing-print sweep   (testHost_context)
#   (118 to 136 are the host context, language layer, output sink and string helpers of story 4.)
#   137 a decline answered with an error value   the decline allocation count and the error-hook tests   (testCustom)
#   138 the reflected call not swapped   the reflected-call tests   (testCustom)
#   139 the reflected call made after an answer   the left-operand-answers test   (testCustom)
#   140 the string rule answering a custom operand   the built-in-on-the-left test   (testCustom)
#   141 the left operand never consulted   the left-operand-answers test   (testCustom)
#   142 a slot not traced   the slot-survives-a-collection test under forced torture and verify   (testCustom)
#   143 a slot written without the barrier   barrier-verify in the forced torture tests   (testCustom)
#   144 an unattached type accepted by make_custom   the unattached-type test   (testCustom)
#   145 a registry not sealed on attach   the seal test   (testCustom)
#   146 a non-thread-safe registry attached to a second context   the binding test   (testCustom)
#   147 type identity compared by name only   the identity test   (testCustom)
#   148 equality not defaulting to identity   the equality table   (testCustom)
#   149 the operation called without the host-function flag   the host-function test   (testCustom)
#   150 the type asked about an error operand   the error-operand test   (testCustom)
#   151 an element's equality error read as not equal   the container-comparison error test   (testCustom)
#   (137 to 151 are the custom kind, its descriptor and its registry of story 5.)
#   152 the compiled print charges no fuel   the loop of prints, fuel against the interpreter's   (testJit_templates)
#   153 a compiled print leaves its operand in place of its value   the loop of prints, whose function returns a print's value   (testJit_templates)
#   154 a compiled long text printed without pacing   the large-print test: pacing polls, fuel, polls, a scope that runs out inside it   (testJit_templates)
#   155 a compiled print does not store its position before the call   the refused delivery, whose error names the print's line   (testJit_templates)
#   156 the interpreter's frame does not stand at a print while it runs   the frame differential over the output programs   (testObserver)
#   157 a compiled printer's guest frame keeps its last poll's identity   the large-print test, the identity of every poll   (testJit_templates)
#   158 the compiled CLEAR_RESULT charges no fuel   the template whose first operation is text, fuel against the interpreter's   (testJit_templates)
#   (152 to 158 are the output operations compiled of spec-template-compiled story 1. 156 is in the
#   interpreter, and only the JIT arm can see it: a poll inside a print is compared with compiled code's.)
#   159 the compiled HALT charges no fuel   the warm template and every other template and script test, fuel against the interpreter's   (testJit_templates)
#   160 a halted return counted as an exit   the warm template called a hundred times: its counted exits, still compiled   (testJit_templates)
#   161 the compiled SET_RESULT skips the error list's rules   the error result a later statement replaces, its discarded record   (testJit_templates)
#   162 the compiled STORE_GLOBAL writes the wrong slot   the top-level variables of a script and of a template, read back   (testJit_templates)
#   163 the compiled DISCARD records nothing   the bare statement whose value is an error, its three entries   (testJit_templates)
#   164 the compiled DISCARD records the wrong position   the bare statement whose value is an error, the position its run ends with   (testJit_templates)
#   165 a raw float stored by STORE_GLOBAL with no box exit   the raw-float box exits, one scenario per operation, exact counts   (testJit_templates)
#   166 the compiled SET_RESULT takes every result as listed   the error result a variable holds: not lost, not recorded   (testJit_templates)
#   167 the halted word names another instruction than the HALT   the template call that ends a run, the position it ends with   (testJit_templates)
#   (159 to 167 are the end of a statement and of a template compiled, spec-template-compiled story 2.)
#   168 the compiled attribute reads its own copy of the container after making the name   the template of attributes, elements and concatenations under relocation torture   (testJit_templates, GRHEAP_RELOCATE+TORTURE); invisible without relocation
#   169 no test for a host-defined operand before a compiled value operation   the host-defined operand of each value operation, its exits and helper calls   (testJit_templates)
#   170 the compiled slice does not stand while it runs   the long operations, the identity of every poll   (testJit_templates)
#   171 a compiled concatenation charges no fuel   the concatenation in a loop, fuel against the interpreter's   (testJit_templates)
#   172 a raw float beside a string-evident operand is never its concatenation   the raw float beside a string, its expected and counted exits   (testJit_templates)
#   173 the interpreter's frame does not stand at a slice while it runs   the long operations, the identity of every poll   (testJit_templates)
#   174 the compiled attribute of a value does not stand while it runs   the long operations' retag and render, the identity of every poll   (testJit_templates)
#   175 the compiled index does not stand while it runs   the long operations' grapheme index, the identity of every poll   (testJit_templates)
#   176 the slow path of a compiled `+` tests only its left operand for a host-defined value (the one test of both, story 4e)   the host-defined operand's "right of +" row   (testJit_templates)
#   177 `apply_kinds` has no entry for INDEX and SLICE   the opcode policy over both kind tables   (testJit_floats)
#   (178, the interpreter standing at an operator of two numbers, is retired: since story 4b a stand
#    lasts only its operation's call and writes nothing unless a poll happens inside it, and an
#    operator of two numbers never polls inside, so the stand cannot reach a later poll)
#   (168 to 177 are the value operations compiled, spec-template-compiled story 3. 173 is in the
#    interpreter and only the JIT arm sees it, as 156.)
#   179 a literal's elements are not live across the allocation of its container   the literal of fresh strings under torture, verify and a moving stack   (testJit_templates)
#   180 a cast to int of an out-of-range float constant is folded anyway   the three corpus scripts of out-of-range constant casts   (testJit_templates)
#   181 the compiled cast to string does not stand while it runs   the cast to string of a large array, the identity of every poll   (testJit_templates)
#   182 a compiled ADOPT does not copy   the literals in a loop, the held row   (testJit_templates)
#   183 the interpreter's frame does not stand at a cast to string while it runs   the cast to string of a large array, the identity of every poll   (testJit_templates)
#   184 the compiled ADOPT does not stand while it copies   the copying ADOPT of a large array, the identity of every poll   (testJit_templates)
#   185 the interpreter's frame does not stand at an ADOPT that copies   the copying ADOPT of a large array, the identity of every poll   (testJit_templates)
#   186 compiled ADOPT does not copy a map   the literals in a loop, the held map   (testJit_templates)
#   187 a compiled cast charges no fuel   the template of casts and literals, its fuel   (testJit_templates)
#   188 the constant-cast fold's upper bound is inclusive   the constant casts at both bounds   (testJit_templates)
#   189 the constant-cast fold's lower bound is exclusive   the constant casts at both bounds   (testJit_templates)
#   190 the array fill stores into a failed allocation's value   the memory sweep over a compiled function's literals   (testJit_templates)
#   191 a map literal's key the memory budget refused is skipped   the memory sweep where only a long map key is refused   (testJit_templates)
#   (179 to 191 are the value-building operations compiled, spec-template-compiled story 4. 183, 185
#    and 191 are in the interpreter (191 in the fill both tiers share) and only the JIT arm sees them, as 156.)
#   192 the warm template leaves compiled code at every compiled call (an exit that counts)   the template policy: zero exits, never discarded   (bench)
#   193 the per-row template leaves at a designed box exit at every call   the template policy: zero expected exits, every compiled call returns   (bench)
#   194 the template policy expects the top level's entry to return   compiled entries returned exactly   (bench)
#   195 the template policy forgets the top level's entry   entries into compiled code exactly   (bench)
#   196 a raw float's print is the designed box exit again   the float-print template's policy: zero exits, every compiled call returns   (bench)
#   197 a raw float's concatenation leaves compiled code after its call   the float-label template's policy: zero exits, every compiled call returns   (bench)
#   (192 to 197 are in the harness, spec-template-compiled story 5: 192 and 193 plant an exit in a template row's program
#    with its output unchanged, 194 and 195 a mistake in the policy that only the new checks of entries and returns see.
#    196 and 197 were the box-exit and the counted-exit policies' until story 4g removed those policies with the exits
#    they described: they are now defects in the compiler and a thunk that only the float rows' policy sees.)
#   198 a poll inside an operation that stands reports the last poll before it, on both tiers   the poll identities located to their lines, each tier on its own   (testJit_templates, both arms)
#   199 a compiled text still stands after its call   the large-print test's template, whose last text is the thunk, the identity of every poll   (testJit_templates)
#   200 an entry into compiled code extends the reservation only for more than one raw float   the reservation's capacity across compiled calls and a nested entry   (testJit_floats)
#   201 an entry into compiled code reads no single argument   the fuel-parity test   (testJit)
#   202 a compiled index still stands after its call   the memory sweep after a short standing operation, the identity of every poll   (testJit_templates)
#   203 the interpreter's index still stands after its call   the memory sweep after a short standing operation, the identity of every poll   (testJit_templates)
#   204 the poll that writes an operation's identity does not end its stand   the scope that runs out inside an attribute's name, the identity of every poll   (testJit_templates)
#   (198 to 204 are spec-template-compiled story 4b: 198 and 204 in the runtime poll both tiers share,
#    199 and 202 in thunks, 203 in the interpreter, 200 and 201 in the entry. 198 is seen by no
#    comparison of the tiers, so its test reads each tier on its own and runs in the JIT=no arm too;
#    203 and 204 only the JIT arm sees.)
#   205 a compiled text the fast path copies charges no fuel   the template of texts and a short string, fuel against the interpreter's   (testJit_templates)
#   206 the fast path copies a print the host's sink is to be given   the loop of texts with a sink, its deliveries   (testJit_templates)
#   207 the fast path takes a print of the pacing interval's own length   the strings at and below the interval, their polls   (testJit_templates)
#   208 the fast path appends to a segment of another tag   the texts and HTML strings that alternate, the rendered output   (testJit_templates)
#   209 the fast path does not move the point a failed print rolls back to   the memory sweep over a long print after a fast one   (testJit_templates)
#   210 the fast path prints a string of two runs as one   the string of two runs after an HTML string, the rendered output   (testJit_templates)
#   211 a map's attribute is the fast path before its name is made   the loop of a map's attribute, the heap objects made   (testJit_templates)
#   212 the fast path's room test leaves out the terminator   the sixteen-byte texts, the count the requirement gives   (testJit_templates)
#   213 the fast path of a string's print does not look at the temporaries' room   the memory sweep over a first print of a string   (testJit_templates)
#   214 a compiled string the fast path copies charges no fuel   the template of texts and a short string, fuel against the interpreter's   (testJit_templates)
#   215 a map's attribute the fast path reads charges no fuel   the loop of a map's attribute, fuel against the interpreter's   (testJit_templates)
#   216 the attribute's fast path reads any value as a map   the loop of a map's attribute beside a string's and a library's   (testJit_templates)
#   217 compiled code is built with a stale hash of an attribute's name   the fast path's assert, in the loop of a map's attribute   (testJit_templates)
#   218 the attribute's fast path asks the main program's constants whether its name is made   a template's m.name after the script made thirty constants, its heap objects   (testJit_templates)
#   (205 to 218 are spec-template-compiled story 4d's fast paths, the second implementation of the common case
#    of a print and of a map's attribute: 205, 214 and 215 in the JIT's helpers, 217 in the emitter, the rest in the
#    vm's fast functions. Those are compiled into a JIT=no build too, but nothing there calls them, and their tests
#    are the JIT arm's, so under JIT=no the cases are skipped like the JIT's own.)
#   219 the interpreter calls an operator of two integers without recording its position   the offset of a division's and a modulo's error   (testEngine)
#   220 the table of natives is never made   the loop of library calls in a compiled function, which asserts no exit   (testNative_calls)
#   221 every entry from the interpreter puts its arrays in a frame for the largest function   the tiny native stack's 16 KiB row   (testJit_calls)
#   222 an output's segments are never grown past the first four   the texts and HTML strings that alternate, their rendered output   (testJit_templates)
#   223 compiled code stores an operation's position with its two words swapped   the template tests' errors, polls and identities   (testJit_templates)
#   224 compiled code passes the execution to a library call's thunk too   the natives of every arity   (testNative_calls)
#   225 a raw float's print that the fast path copies charges no fuel   the prints, concatenations and casts of the special values, fuel   (testJit_templates)
#   226 the slow print of a raw float reads the float's bits as an integer   the raw float printed to the host's sink, its deliveries   (testJit_templates)
#   227 a raw float's concatenation with a string puts the float on the wrong side   the prints, concatenations and casts of the special values, their text   (testJit_templates)
#   228 the float arithmetic of a fused `+` charges no fuel   the printed sum of a raw float and a value word, fuel   (testJit_templates)
#   229 the concatenation of a fused `+` is given the float's position as the string's   the prints, concatenations and casts of the special values, their text   (testJit_templates)
#   230 a raw float's cast to string charges no fuel   the prints, concatenations and casts of the special values, fuel   (testJit_templates)
#   231 a store that a jump lands on is taken for a store of the string constant before it   the string the scan cannot see, its counted exits   (testJit_templates)
#   232 a loop that iterates the collection itself   the shared-collection test   (testCustom_ops)
#   233 an iterator's done read as a failure   the iteration tests   (testCustom_ops)
#   234 a begin that answers anything is an iterator   the begin-answers-something-else test   (testCustom_ops)
#   235 a next that errors, swallowed   the next-errors test   (testCustom_ops)
#   236 a next that declines ends the loop without an error   the next-declines test   (testCustom_ops)
#   237 a declined call that is null   the call-with-no-entry-or-a-decline test   (testCustom_ops)
#   238 the last argument of a call dropped   the callable-object test: the argument count   (testCustom_ops)
#   239 a declined cast with no default   the declined-cast tests   (testCustom_ops)
#   240 a cast to bool that does not ask the type   the cast-is-asked-first test   (testCustom_ops)
#   241 an instance that stands for an error asked about its truth   the marked-instance-is-falsy test   (testCustom_ops)
#   242 a truthy that declines is false   the every-truth-test test   (testCustom_ops)
#   243 `!` that does not ask the type   the every-truth-test test   (testCustom_ops)
#   244 a jump on false that does not ask the type   the every-truth-test test   (testCustom_ops)
#   245 a jump on true that does not ask the type   the every-truth-test test   (testCustom_ops)
#   246 `&&` that does not ask the type   the every-truth-test test   (testCustom_ops)
#   247 `||` that does not ask the type   the every-truth-test test   (testCustom_ops)
#   248 a print whose segments lose their tags   the print-keeps-its-tag test   (testCustom_ops)
#   249 a print with no fallback to to_string   the print-falls-back test   (testCustom_ops)
#   250 a non-string text accepted   the not-a-string tests   (testCustom_ops)
#   251 a failed render that keeps what it appended   the not-a-string tests: nothing partial   (testCustom_ops)
#   252 an element rendered with print   the element-text test   (testCustom_ops)
#   253 an instance that stands for an error is asked for its operators   the contamination test   (testCustom_ops)
#   254 the truth of an error instance makes an engine error   the not-an-engine-error test   (testCustom_ops)
#   255 an error mark that cannot be cleared   the mark-is-cleared test   (testCustom_ops)
#   256 the error mark read as set on a value that is not an instance   the flag-is-refused test   (testCustom_ops)
#   257 a host attribute that wins over a built-in name   the built-in-name-wins test   (testCustom_ops)
#   258 an attribute registered for a map   the maps-and-libraries test   (testCustom_ops)
#   259 a clash of host attributes not checked at attach   the attribute-clash test   (testCustom_ops)
#   260 a declined host attribute that is null   the declined-host-attribute test   (testCustom_ops)
#   261 a descriptor read past its size   the descriptor-sized-for-the-first-version test   (testCustom_ops)
#   262 a table hash that changes for a type that uses none of the new operations   the hash-as-before-story-6 test   (testCustom_ops)
#   263 a done from an operation that is not an iterator's next taken as a decline   the done-from-anything-but-next test   (testCustom_ops)
#   264 a truthy that declines makes an error   the decline-path allocation test   (testCustom_ops)
#   265 a built-in attribute read that allocates once a registry is attached   the built-in-attribute allocation test   (testCustom_ops)
#   266 the native depth of compiled code's records not handed back for a callback   the tight native-depth differential   (testCustom_ops)
#   267 an iterator's next called without the host-function flag   the host-function test of every new operation   (testCustom_ops)
#   268 a lookup in a sealed registry that writes it   ThreadSanitizer on the shared-registry test (the TSan arm only: not in the quick set)   (testCustom_ops)
#   (232 to 268 are spec-tang-host-extensibility story 6's, all in testCustom_ops run plain: iteration 232 to 236, calls 237 and 238, casts 239 and 240, truthiness 241 to 247, print and to-string 248 to 252, the error mark 253 to 256, attributes of built-in kinds 257 to 260 and 265, the descriptor 261 and 262, the new status 263, the bracket 264, 266 and 267; 268 is seen only by ThreadSanitizer, `make check-planted-tsan`. 137, 138, 139, 141, 150 and 151 were regenerated because this story moved their context.)
#   269 a truthy's error dropped without a trace   the truthy-error test   (testCustom_ops)
#   270 a cast answer of the wrong kind accepted   the wrong-kind-cast test   (testCustom_ops)
#   271 a cast to bool of an instance marked as an error that asks the type   the marked-instance-cast test   (testCustom_ops)
#   272 a truthy's error set through the call object overwritten by the boolean   the truthy-error-through-the-call-object test   (testCustom_ops)
#   273 an iterator step's error set with the status OK taken for an item   the next-error-with-status-ok test   (testCustom_ops)
#   274 a cast of a host-defined value that does not stand at the operation   the polling-cast test   (testCustom_ops)
#   275 a refused record that is not marked as an error   the refused-record test   (testCustom_ops)
#   276 a generator member missing from the one table   the every-built-in-member-name test   (testCustom_ops)
#   277 the native-depth units lent for a callback never taken back   the refused-record test: the native depth afterwards   (testCustom_ops)
#   (269 to 277 are the review of story 6's: a truthy's error recorded as discarded 269, the kind of a cast's answer 270, a marked instance cast to bool 271, a truthy's error through the call object 272, an iterator step's 273, a cast that polls 274, a refused record 275 and 277 (277 only the JIT arm sees), the one table of a generator's members 276. 181 and 183 were regenerated for the cast's context.)
#   300 a PENDING that asked for nothing taken for an answer   the pending-with-no-request misuse test   (testCustom_reentry)
#   301 a request followed by OK still stands as an answer   the request-then-OK misuse test   (testCustom_reentry)
#   302 a second request in one callback is accepted   the two-requests misuse test   (testCustom_reentry)
#   303 an opaque callback can ask for a guest call   the opaque-callback misuse test   (testCustom_reentry)
#   304 the resumable mask is not consulted when a callback is entered   the opaque-callback misuse test   (testCustom_reentry)
#   305 a descriptor may list truthy as resumable   the registration checks test   (testCustom_reentry)
#   306 a descriptor may list an operation it does not implement as resumable   the registration checks test   (testCustom_reentry)
#   307 a request may name the entry one past the table   the bad-continuation-index misuse test   (testCustom_reentry)
#   308 the callback's activation record is not left at PENDING   the depth, records and scopes readings inside the callee test   (testCustom_reentry)
#   309 the callback's handle scope is not closed at PENDING   the depth, records and scopes readings inside the callee test   (testCustom_reentry)
#   310 the continuation's scope has the serial of the entry's   the stale-handle-in-the-continuation misuse test   (testCustom_reentry)
#   311 an unwound run keeps its pending records   the limit-unwind test (no record survives)   (testCustom_reentry)
#   312 the record a continuation was called from stays on the chain   the pending-depth check after every run   (testCustom_reentry)
#   313 a callee that cannot be entered ends the run   the callee-that-cannot-be-entered test   (testCustom_reentry)
#   314 the state and the result are swapped   the map, sort and operation tests   (testCustom_reentry)
#   315 the continuation's call object has no arguments   the map, sort and operation tests   (testCustom_reentry)
#   316 a callee refused for its argument count is Host function failed   the callee-that-cannot-be-entered test   (testCustom_reentry)
#   317 a step before the record's is called again on resume   the dispatch-step log tests   (testCustom_reentry)
#   318 a decline from a continuation is a failure   the dispatch-step log tests   (testCustom_reentry)
#   319 the steps of the equal fallback of != are 0 and 1   the != chain log test   (testCustom_reentry)
#   320 DONE from an iterator step's continuation is a failure   the done-from-the-continuation test   (testCustom_reentry)
#   321 an assignment that answers nothing after a request is null   the assignment-of-no-answer test and the generated operation test   (testCustom_reentry)
#   322 the pending record's state slot is not traced   the collection-while-the-callee-runs tests under torture   (testCustom_reentry)
#   323 a pointer is stored into the pending record without the barrier   the collection-while-the-callee-runs tests under torture and verify   (testCustom_reentry)
#   324 the suspended instruction's top operand is dropped from the stack   the matrix and operation tests   (testCustom_reentry)
#   325 the callee's RET pushes its value for the instruction instead of delivering it   every test that asks for a function   (testCustom_reentry)
#   326 ITER_NEXT run again does not step over its own word   the computed-iterator test   (testCustom_reentry)
#   327 a continuation is not charged as a native call   the golden fuel totals test   (testCustom_reentry)
#   328 a request is not charged as the CALL of its callee is   the golden fuel totals test   (testCustom_reentry)
#   329 a snapshot is taken at a pause with a pending record   the snapshot-at-a-pause test   (testCustom_reentry)
#   330 a request from a site inside a C loop makes a pending record   the embedded-site tests (the nested statistic)   (testCustom_reentry)
#   331 a nested request may name a callee that is not a function   the non-function-callee embedded test   (testCustom_reentry)
#   332 compiled code calls a resumable native   the resumable-native-is-an-exit test   (testNative_calls)
#   333 a template callee stopped by its own budget is not the callee's value   the template-callee-limit test   (testCustom_reentry)
#   334 a template callee's text is pushed for the instruction   the callable-object-runs-a-template test   (testCustom_reentry)
#   335 a compiled callee's return is pushed for the instruction   the tests that run at threshold 1   (testCustom_reentry)
#   (300 to 335 are spec-tang-host-extensibility story 9's: the protocol by which a host callback asks the engine to call guest code and is called again with the result. 332 and 335 plant a defect only a JIT build has. Each patch is one exact-string edit, applied with no fuzz, so one whose context moves is made again from the same edit.)
#   (225 to 231 are spec-template-compiled story 4g's, a raw float's print, concatenation and cast to string compiled:
#    225 in the JIT's helpers, 226 and 230 in thunks, 227 to 229 and 231 in the emitter and its analysis.)
#   (219 to 224 are spec-template-compiled story 4e's: 219 in the interpreter's integer path of the binary operators,
#    run in both builds; 222 in the output both tiers share, seen by a test of the JIT arm; the rest the JIT's own.)
#   (Library defects, all of them, in src/: the host-described values of spec-tang-host-extensibility story 2. 84 and 85 are
#   the two a collector finds and a plain run does not; each of the rest is a rule of the builder or the materializer that a
#   test states from the outside. 91 to 94, 96, 97 and 100 are in the builder (src/vm/description.c) and 90, 98 and 99 are the
#   materializer's own second look at what the builder already checked (src/vm/materialize.c).)
#   (Library defects: 66, 67, 73, 74, 75, 79, 80 and 83. Harness defects, in bench/bench.c and not in the library: 65, 68 to 72,
#   76 to 78, 81, 82 and 192 to 197; 65 needs the fresh and the steady selection both to abort.)
#   (65 is in the harness and not in the library: the harness checks every float and integer program's
#   answer against a C reference built with -ffp-contract=off, and a check never seen to fail may check nothing.)
#   (48 to 64 are the compiled float and division operations of story 7. 57 is the one that is a
#   performance defect: nothing is wrong in any output, and the allocation slope is what fails.)
#   (35 to 47 are the compiled float of spec-runtime-float story 6. The story's third defect,
#   a raw slot written before its conversion finished, is 37 in the form the heap sees (a raw
#   word in the conversion's root cell across a collection) and 47 with the failing-rebuild
#   seam (raw words copied into the guest frame before a rebuild that is then refused).)
#
# Cases 13, 14, 15, 20, 27, 46, 117 and 168 (`--relocate`) are caught only by runtime-heap's relocation
# torture, which moves every unpinned object at every collection: a reference
# that is visited and not updated is a defect nothing else can see, because the
# collector never moves anything otherwise. They are built and run against the
# runtime-heap of PLANTED_RELOC_PREFIX, which must be a relocation build, and a
# planted case that is caught is also run with torture but without relocation,
# where it must PASS: that is what shows relocation, and not the torture it rides
# with, to be the instrument.
#
# The copy is of the working tree (sources, tests, corpus, documentation) and
# nothing in the working tree is modified. Each patch is applied with `patch
# --fuzz=0` and the script checks that it matched and that the file changed, so
# a patch that applies to nothing fails the script instead of reading as a gate
# that held. The copy is removed when the script ends (PLANTED_KEEP=1 keeps it).
#
# Usage: PLANTED_PREFIX=<prefix> PLANTED_LIBDIR=<dir> tools/check-planted.sh [--quick | --slow | --all | --relocate] [--selftest] [case...]
#   --quick     the cases that finish in about a minute (03 to 09, 11, 12, 16, 17, 19, 21 to 26, 28 to 45, 47 to 116, 118 to 132 and 134 to 167 and 169 to 177 and 179 to 267, 269 to 277 and 300 to 335; 10, 18 and 178 are retired); `make test` runs these
#   --slow      the torture cases (01, 02); `make test-torture` runs these
#   --all       every case (the default); `make check-planted`
#   --relocate  cases 13, 14, 15, 20, 27, 46, 117 and 168, against PLANTED_RELOC_PREFIX and
#               PLANTED_RELOC_LIBDIR (a relocation runtime-heap); `make
#               check-planted-relocate` runs these, and `make test-relocate`
#               calls that
#   --selftest  prove the script itself: a patch that applies to nothing fails
#               it, and a patch that breaks nothing is reported as not caught
# PLANTED_JIT is yes (the default) or no, the JIT= the library is built with. The
# copy is built the same way, in its own tree (release-nojit for no). Cases 08, 09,
# 15 to 83, 152 to 197, 199 to 218, 225 to 231 and 266 plant a defect in the JIT, or in code only the JIT calls (156, 173, 183, 185, 191, 203 to 218 ones only the JIT arm sees; 192 to 197 in harness rows only a JIT build has), which a JIT=no build does not contain, so
# with no they are SKIPPED, loudly, and counted as skipped and never as caught:
# the summary line gives both numbers, and a run in which nothing was caught
# fails (an all-skipped run proves nothing). With yes every case runs.
# PLANTED_PREFIX is the PREFIX the dependencies were installed with (empty for
# none) and PLANTED_LIBDIR the directory their shared libraries are in; the
# pkg-config path is the caller's, as for every make in this library.

set -u
# Every explicit --gtest_filter below selects its intended detector. Inherited
# sharding, repetition, or disabled-test settings cannot narrow a planted
# control (or turn a zero-test control into an apparent pass).
unset GTEST_FILTER GTEST_TOTAL_SHARDS GTEST_SHARD_INDEX GTEST_SHARD_STATUS_FILE
unset GTEST_REPEAT GTEST_ALSO_RUN_DISABLED_TESTS

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"
PATCHES="$ROOT/tests/planted"
WORK="$ROOT/build/planted/copy"
LOG="$ROOT/build/planted/build.log"
JOBS="${PLANTED_JOBS:-8}"
PREFIX="${PLANTED_PREFIX:-}"
LIBDIR="${PLANTED_LIBDIR:-}"
JIT="${PLANTED_JIT:-yes}"
case "$JIT" in
  yes) TREE=release ;;
  no) TREE=release-nojit ;;
  *) printf 'check-planted: PLANTED_JIT must be yes or no, not %s\n' "$JIT" >&2; exit 2 ;;
esac
# Cases whose defect is in code only a JIT=yes build contains.
JIT_ONLY="08-jit-wrong-tag 09-jit-skipped-fuel 15-push-reads-args-first 16-push-omits-call-fuel 17-depth-test-off-by-one 19-deopt-ignores-failed-rebuild 20-poll-helper-copies-the-guest-frame-back 21-compile-hook-does-not-refuse 22-remembered-exit-counts 23-call-passes-the-entry-flag-as-one 24-discard-destroys-the-code-directly 25-native-record-not-closed 26-native-depth-miscount 27-arguments-not-pinned 28-stale-stack-over-a-nested-activation 29-unwind-status-read-as-ok 30-member-load-by-name-not-charged 31-deep-push-refused 32-float-add-one-ulp-off-in-the-jit-arm 33-float-subnormal-flushed-in-the-jit-arm 34-float-nan-where-a-number-belongs-in-the-jit-arm 35-pool-boxes-not-reported-as-roots 36-reservation-short-by-one 37-raw-word-in-the-root-cell-across-a-collection 38-reverse-with-no-type-test 39-pop-does-not-retract 40-unwind-converts 41-box-at-every-slow-poll 42-merge-conflict-ignored 43-float-constant-through-32-bits 44-pool-not-let-go-at-host-return 45-failed-push-keeps-the-extension 46-pool-box-reported-through-a-copy 47-failed-rebuild-leaves-raw-words-in-the-guest-frame 48-float-divide-guard-tests-the-dividend 49-integer-operand-converted-unsigned 50-unbox-with-no-kind-test 51-ge-and-le-as-the-negation-of-lt-and-gt 52-as-int-upper-bound-inclusive 53-integer-quotient-not-range-guarded 54-result-slot-written-before-the-second-guard 55-mixed-subtraction-operands-swapped 56-float-operation-adds-no-fuel 57-arithmetic-on-raw-operands-is-an-exit 58-integer-converted-from-its-tagged-word 59-unary-minus-is-zero-minus-x 60-unbox-with-no-pointer-test 61-as-int-lower-bound-too-wide 62-nan-divisor-read-as-zero 63-modulo-zero-divisor-guard-removed 64-neg-compare-cast-mod-div-add-no-fuel 65-bench-reference-one-off 66-every-push-looks-the-callee-up 67-every-extension-is-recorded 68-bench-integer-reference-one-off 69-bench-steady-expectation-one-off 70-bench-family-expectation-one-off 71-bench-compiled-row-is-interpreted 72-bench-discard-expectation-wrong 73-expected-exit-counts-toward-the-limit 74-static-decline-ignores-the-loop 75-static-decline-ignores-a-real-exit 76-bench-expected-exit-count-wrong 77-bench-decline-not-expected 78-bench-counted-exit-count-wrong 79-static-decline-ignores-a-normal-return 80-exit-sites-from-the-functions-record 81-bench-exit-kinds-sum-omits-remembered 82-bench-declined-callee-entries-ignored 83-every-compiled-call-looks-the-callee-up 152-print-skips-its-fuel 153-print-leaves-its-operand 154-long-text-printed-without-pacing 155-print-position-not-stored 156-interpreted-print-not-at-the-print 157-compiled-print-not-at-the-print 158-clear-result-skips-its-fuel 159-halt-skips-its-fuel 160-halted-return-counted-as-an-exit 161-set-result-skips-the-lost-result 162-store-global-wrong-slot 163-discard-skips-the-swallow 164-discard-wrong-position 165-store-global-reads-a-raw-float 166-set-result-always-listed 167-halted-word-names-the-wrong-halt 168-attribute-operand-not-held 169-custom-operand-guard-removed 170-compiled-slice-identity-not-set 171-concatenation-skips-its-fuel 172-raw-float-beside-a-string-never-a-concatenation 173-interpreted-slice-not-at-the-slice 174-compiled-attribute-identity-not-set 175-compiled-index-identity-not-set 176-add-tests-only-its-left-operand 177-apply-kinds-misses-index-and-slice 179-literal-elements-not-live 180-out-of-range-constant-cast-folded 181-compiled-cast-to-string-not-at-the-cast 182-adopt-does-not-copy 183-interpreted-cast-to-string-not-at-the-cast 184-compiled-adopt-not-at-the-adopt 185-interpreted-adopt-not-at-the-adopt 186-adopt-copies-no-map 187-cast-skips-its-fuel 188-constant-cast-upper-bound-inclusive 189-constant-cast-lower-bound-exclusive 190-array-fill-into-a-failure 191-refused-map-key-skipped 192-bench-warm-template-exits 193-bench-row-template-box-exit 194-bench-template-top-level-entry-returns 195-bench-template-top-level-entry-forgotten 196-bench-raw-float-print-is-a-box-exit-again 197-bench-raw-float-concatenation-leaves-after-its-call 199-compiled-print-stands-past-its-call 200-entry-extends-no-single-raw-float 201-entry-reads-no-single-argument 202-compiled-index-stands-past-its-call 203-interpreted-index-stands-past-its-call 204-poll-does-not-end-the-stand 205-fast-text-skips-its-fuel 206-fast-print-ignores-the-sink 207-fast-print-takes-the-pacing-length 208-fast-print-ignores-the-tag 209-fast-print-does-not-commit 210-fast-print-of-two-runs 211-fast-attribute-before-its-name 212-fast-print-room-without-the-terminator 213-fast-string-print-ignores-the-temporaries 214-fast-string-print-skips-its-fuel 215-fast-attribute-skips-its-fuel 216-fast-attribute-of-any-kind 217-fast-attribute-stale-hash 218-fast-attribute-reads-the-callers-constants 220-natives-never-made 221-entry-arrays-sized-for-the-largest-frame 222-output-segments-not-grown-past-four 223-site-word-halves-swapped 224-execution-passed-to-library-calls 225-fast-float-print-skips-its-fuel 226-slow-float-print-reads-its-bits-as-an-integer 227-raw-float-concatenation-sides-swapped 228-fused-float-sum-skips-its-fuel 229-fused-concatenation-of-the-other-operand 230-float-to-string-skips-its-fuel 231-string-store-a-jump-lands-on-is-evidence 266-host-depth-not-handed-back 277-lent-depth-never-returned 332-compiled-call-of-a-resumable-member-taken 335-compiled-return-does-not-deliver"

PREFIX_ARG=""
if [ -n "$PREFIX" ]; then
  PREFIX_ARG="PREFIX=$PREFIX"
fi

MODE=all
SELFTEST=0
SELECTION_SELFTEST=0
NAMED=""
for arg in "$@"; do
  case "$arg" in
    --quick) MODE=quick ;;
    --slow) MODE=slow ;;
    --all) MODE=all ;;
    --relocate) MODE=relocate ;;
    --selftest) SELFTEST=1 ;;
    --selftest-selection) SELECTION_SELFTEST=1 ;;
    -*) printf 'check-planted: unknown option %s\n' "$arg" >&2; exit 2 ;;
    *) NAMED="$NAMED $arg" ;;
  esac
done
if [ -z "$LIBDIR" ] && [ "$SELECTION_SELFTEST" -ne 1 ]; then
  printf 'check-planted: set PLANTED_LIBDIR to the directory the dependencies'"'"' shared libraries are in, and PLANTED_PREFIX to the PREFIX they were installed with (make check-planted does both)\n' >&2
  exit 2
fi

# The cases that need a runtime-heap that moves objects.
RELOCATE_CASES="13-temporaries-through-a-copy 14-array-storage-through-a-copy 15-push-reads-args-first 20-poll-helper-copies-the-guest-frame-back 27-arguments-not-pinned 46-pool-box-reported-through-a-copy 117-persistent-reported-through-a-copy 168-attribute-operand-not-held"
# What a relocation case runs under: relocation and torture together (a move at
# every GC point). The "relocation is the instrument" check removes the first.
RELOC_ENV="GRHEAP_TORTURE=1 GRHEAP_RELOCATE=1"
QUICK="03-order-dependent-decide 04-native-never-polls 05-frame-slot-mismatch 06-wrong-operator 07-silent-runner 08-jit-wrong-tag 09-jit-skipped-fuel 11-host-pointer-no-hook 12-skipped-output-capture 16-push-omits-call-fuel 17-depth-test-off-by-one 19-deopt-ignores-failed-rebuild 21-compile-hook-does-not-refuse 22-remembered-exit-counts 23-call-passes-the-entry-flag-as-one 24-discard-destroys-the-code-directly 25-native-record-not-closed 26-native-depth-miscount 28-stale-stack-over-a-nested-activation 29-unwind-status-read-as-ok 30-member-load-by-name-not-charged 31-deep-push-refused 32-float-add-one-ulp-off-in-the-jit-arm 33-float-subnormal-flushed-in-the-jit-arm 34-float-nan-where-a-number-belongs-in-the-jit-arm 35-pool-boxes-not-reported-as-roots 36-reservation-short-by-one 37-raw-word-in-the-root-cell-across-a-collection 38-reverse-with-no-type-test 39-pop-does-not-retract 40-unwind-converts 41-box-at-every-slow-poll 42-merge-conflict-ignored 43-float-constant-through-32-bits 44-pool-not-let-go-at-host-return 45-failed-push-keeps-the-extension 47-failed-rebuild-leaves-raw-words-in-the-guest-frame 48-float-divide-guard-tests-the-dividend 49-integer-operand-converted-unsigned 50-unbox-with-no-kind-test 51-ge-and-le-as-the-negation-of-lt-and-gt 52-as-int-upper-bound-inclusive 53-integer-quotient-not-range-guarded 54-result-slot-written-before-the-second-guard 55-mixed-subtraction-operands-swapped 56-float-operation-adds-no-fuel 57-arithmetic-on-raw-operands-is-an-exit 58-integer-converted-from-its-tagged-word 59-unary-minus-is-zero-minus-x 60-unbox-with-no-pointer-test 61-as-int-lower-bound-too-wide 62-nan-divisor-read-as-zero 63-modulo-zero-divisor-guard-removed 64-neg-compare-cast-mod-div-add-no-fuel 65-bench-reference-one-off 66-every-push-looks-the-callee-up 67-every-extension-is-recorded 68-bench-integer-reference-one-off 69-bench-steady-expectation-one-off 70-bench-family-expectation-one-off 71-bench-compiled-row-is-interpreted 72-bench-discard-expectation-wrong 73-expected-exit-counts-toward-the-limit 74-static-decline-ignores-the-loop 75-static-decline-ignores-a-real-exit 76-bench-expected-exit-count-wrong 77-bench-decline-not-expected 78-bench-counted-exit-count-wrong 79-static-decline-ignores-a-normal-return 80-exit-sites-from-the-functions-record 81-bench-exit-kinds-sum-omits-remembered 82-bench-declined-callee-entries-ignored 83-every-compiled-call-looks-the-callee-up 84-container-held-across-a-childs-allocation 85-raw-word-in-a-temp-cell 86-string-tag-dropped 87-map-key-tag-dropped 88-materialization-not-charged 89-in-run-arena-from-the-process-allocator 90-materializer-depth-unchecked 91-builder-depth-unchecked 92-builder-node-limit-unchecked 93-builder-byte-limit-unchecked 94-second-parent-allowed 95-reach-cached 96-no-ancestor-check 97-no-self-attach-check 98-materializer-node-limit-unchecked 99-materializer-byte-limit-unchecked 100-utf8-unchecked 101-materializer-node-ceiling-ignored 102-materializer-byte-ceiling-ignored 103-ref-of-another-description-accepted 104-persistent-not-rooted 105-handle-serial-unchecked 106-scope-limit-unchecked 107-segment-tag-flattened 108-persistent-table-leaked 109-persistent-limit-unchecked 110-no-gc-function-collects 111-held-libraries-leaked 112-owned-members-leaked 113-frame-pop-keeps-handles 114-answer-read-after-the-scope-closes 115-stored-container-aliased 116-scope-serial-reused 118-template-output-delivered 119-delivered-point-not-advanced 120-refused-print-not-rolled-back 121-language-layer-before-the-program 122-language-layer-leaked 123-language-layer-read-afresh 124-language-layer-not-sealed 125-host-context-shared 126-render-ignores-tags 127-concat-loses-second-runs 128-substring-one-too-many 129-value-factory-without-context 130-sink-may-reenter 131-snapshot-delivery-point 132-refused-constant-print-not-rolled-back 134-snapshot-layer-3-is-the-built-ins 135-language-layer-not-retained 136-delivered-before-the-print-is-whole 137-decline-answered-with-an-error-value 138-reflected-call-not-swapped 139-reflected-call-after-an-answer 140-string-rule-answers-a-custom-operand 141-left-operand-never-consulted 142-slot-not-traced 143-slot-written-without-the-barrier 144-unattached-type-accepted 145-registry-not-sealed-on-attach 146-unsafe-registry-attached-twice 147-type-identity-by-name-only 148-equality-not-identity 149-operation-without-the-host-flag 150-error-operand-asked-of-the-type 151-element-equal-error-read-as-false 152-print-skips-its-fuel 153-print-leaves-its-operand 154-long-text-printed-without-pacing 155-print-position-not-stored 156-interpreted-print-not-at-the-print 157-compiled-print-not-at-the-print 158-clear-result-skips-its-fuel 159-halt-skips-its-fuel 160-halted-return-counted-as-an-exit 161-set-result-skips-the-lost-result 162-store-global-wrong-slot 163-discard-skips-the-swallow 164-discard-wrong-position 165-store-global-reads-a-raw-float 166-set-result-always-listed 167-halted-word-names-the-wrong-halt 169-custom-operand-guard-removed 170-compiled-slice-identity-not-set 171-concatenation-skips-its-fuel 172-raw-float-beside-a-string-never-a-concatenation 173-interpreted-slice-not-at-the-slice 174-compiled-attribute-identity-not-set 175-compiled-index-identity-not-set 176-add-tests-only-its-left-operand 177-apply-kinds-misses-index-and-slice 179-literal-elements-not-live 180-out-of-range-constant-cast-folded 181-compiled-cast-to-string-not-at-the-cast 182-adopt-does-not-copy 183-interpreted-cast-to-string-not-at-the-cast 184-compiled-adopt-not-at-the-adopt 185-interpreted-adopt-not-at-the-adopt 186-adopt-copies-no-map 187-cast-skips-its-fuel 188-constant-cast-upper-bound-inclusive 189-constant-cast-lower-bound-exclusive 190-array-fill-into-a-failure 191-refused-map-key-skipped 192-bench-warm-template-exits 193-bench-row-template-box-exit 194-bench-template-top-level-entry-returns 195-bench-template-top-level-entry-forgotten 196-bench-raw-float-print-is-a-box-exit-again 197-bench-raw-float-concatenation-leaves-after-its-call 198-poll-inside-an-operation-reports-the-last-poll 199-compiled-print-stands-past-its-call 200-entry-extends-no-single-raw-float 201-entry-reads-no-single-argument 202-compiled-index-stands-past-its-call 203-interpreted-index-stands-past-its-call 204-poll-does-not-end-the-stand 205-fast-text-skips-its-fuel 206-fast-print-ignores-the-sink 207-fast-print-takes-the-pacing-length 208-fast-print-ignores-the-tag 209-fast-print-does-not-commit 210-fast-print-of-two-runs 211-fast-attribute-before-its-name 212-fast-print-room-without-the-terminator 213-fast-string-print-ignores-the-temporaries 214-fast-string-print-skips-its-fuel 215-fast-attribute-skips-its-fuel 216-fast-attribute-of-any-kind 217-fast-attribute-stale-hash 218-fast-attribute-reads-the-callers-constants 219-integer-operator-call-not-synced 220-natives-never-made 221-entry-arrays-sized-for-the-largest-frame 222-output-segments-not-grown-past-four 223-site-word-halves-swapped 224-execution-passed-to-library-calls 225-fast-float-print-skips-its-fuel 226-slow-float-print-reads-its-bits-as-an-integer 227-raw-float-concatenation-sides-swapped 228-fused-float-sum-skips-its-fuel 229-fused-concatenation-of-the-other-operand 230-float-to-string-skips-its-fuel 231-string-store-a-jump-lands-on-is-evidence 232-loop-uses-the-collection-as-its-iterator 233-iterator-done-is-a-failure 234-begin-answers-anything 235-next-error-is-not-the-loops-value 236-next-decline-ends-the-loop-silently 237-declined-call-is-null 238-last-argument-dropped 239-declined-cast-has-no-default 240-bool-cast-skips-the-type 241-error-instance-asks-the-type 242-truthy-decline-is-false 243-not-skips-the-type 244-jmp-false-skips-the-type 245-jmp-true-skips-the-type 246-and-skips-the-type 247-or-skips-the-type 248-printed-answer-loses-its-tags 249-print-without-the-fallback 250-non-string-text-accepted 251-failed-render-leaves-its-part 252-element-rendered-with-print 253-marked-instance-is-asked-for-operators 254-error-instance-truth-makes-an-error 255-error-mark-cannot-be-cleared 256-error-reader-true-for-anything 257-host-attribute-before-the-builtin 258-attribute-on-a-map 259-attribute-clash-not-checked 260-declined-attribute-is-null 261-descriptor-read-past-its-size 262-hash-counts-the-unused-operations 263-done-from-another-operation-is-a-decline 264-truthy-decline-allocates 265-builtin-attribute-read-allocates 266-host-depth-not-handed-back 267-iterator-next-not-in-the-bracket 269-truth-error-not-recorded-as-discarded 270-cast-answer-kind-unchecked 271-marked-bool-cast-asks-the-type 272-truthy-error-overwritten-by-its-bool 273-iterator-error-with-status-ok-is-an-item 274-custom-cast-does-not-stand 275-refused-record-not-an-error-answer 276-rng-member-dropped-from-the-table 277-lent-depth-never-returned 300-pending-with-no-request-accepted 301-request-kept-after-another-status 302-second-request-accepted 303-opaque-callback-request-accepted 304-resumable-mask-ignored 305-truthy-bit-accepted 306-unimplemented-operation-bit-accepted 307-continuation-index-off-by-one 308-callback-record-held-across-the-callee 309-callback-scope-left-open-at-pending 310-scope-serial-reused-by-the-continuation 311-unwind-leaves-the-pending-chain 312-consumed-record-not-let-go 313-error-result-unwinds-the-run 314-continuation-given-the-result-as-its-state 315-continuation-given-no-arguments 316-arity-mismatch-is-host-failed 317-earlier-step-called-again 318-continuation-decline-is-host-failed 319-not-equal-fallback-steps-not-offset 320-continuation-done-ignored 321-setter-no-answer-rule-lost-after-resume 322-pending-record-state-not-traced 323-pending-record-store-without-the-barrier 324-operands-dropped-while-the-callee-runs 325-return-does-not-deliver 326-iter-next-target-lost-after-resume 327-continuation-not-charged 328-request-not-charged 329-snapshot-with-a-pending-record-written 330-embedded-site-suspends 331-embedded-site-accepts-a-non-function 332-compiled-call-of-a-resumable-member-taken 333-limit-unwound-template-callee-not-delivered 334-template-halt-does-not-deliver 335-compiled-return-does-not-deliver"
SLOW="01-missing-root 02-missing-gc-store"

if [ "$MODE" = relocate ]; then
  PREFIX="${PLANTED_RELOC_PREFIX:-}"
  LIBDIR="${PLANTED_RELOC_LIBDIR:-}"
  if [ -z "$PREFIX" ] || [ -z "$LIBDIR" ]; then
    printf 'check-planted: --relocate needs PLANTED_RELOC_PREFIX and PLANTED_RELOC_LIBDIR (the prefix of a runtime-heap built with RELOCATE=yes, and its library directory)\n' >&2
    exit 2
  fi
  PREFIX_ARG="PREFIX=$PREFIX"
fi

cleanup() {
  if [ "${PLANTED_KEEP:-0}" != 1 ]; then
    rm -rf "$ROOT/build/planted"
  fi
}
trap cleanup EXIT INT TERM

# The test that must notice each patch: the make target that builds it (or the
# runner script for the oracle), and the command that runs it in the copy.
APPS="$WORK/build/linux/$TREE/apps"
LDPATH="$APPS:$LIBDIR"

target_of() {
  case "$1" in
    01-*|02-*) echo "build/linux/$TREE/apps/testExecute_complex" ;;
    03-*|05-*|08-*|32-*|33-*|34-*) echo "build/linux/$TREE/apps/testObserver" ;;
    09-*|201-*) echo "build/linux/$TREE/apps/testJit" ;;
    219-*) echo "build/linux/$TREE/apps/testEngine" ;;
    15-*|16-*|17-*|19-*|20-*|21-*|22-*|23-*|24-*|31-*|83-*|221-*) echo "build/linux/$TREE/apps/testJit_calls" ;;
    25-*|26-*|27-*|28-*|29-*|30-*|220-*|224-*) echo "build/linux/$TREE/apps/testNative_calls" ;;
    18-*) echo "build/linux/$TREE/apps/testObserver" ;;
    35-*|36-*|37-*|38-*|39-*|40-*|41-*|42-*|43-*|44-*|45-*|46-*|47-*|66-*|67-*|80-*) echo "build/linux/$TREE/apps/testJit_floats" ;;
    48-*|49-*|50-*|51-*|52-*|53-*|54-*|55-*|56-*|57-*|58-*|59-*|60-*|61-*|62-*|63-*|64-*|73-*|74-*|75-*|79-*) echo "build/linux/$TREE/apps/testJit_arith" ;;
    11-*|12-*) echo "build/linux/$TREE/apps/testSnapshot" ;;
    65-*|68-*|69-*|70-*|71-*|72-*|76-*|77-*|78-*|81-*|82-*|192-*|193-*|194-*|195-*|196-*|197-*) echo "build/linux/$TREE/apps/bench/bench" ;;
    04-*) echo "build/linux/$TREE/apps/testNative_gate" ;;
    84-*|85-*|86-*|87-*|88-*|89-*|90-*|95-*|98-*|99-*|101-*|102-*) echo "build/linux/$TREE/apps/testHost_values" ;;
    91-*|92-*|93-*|94-*|96-*|97-*|100-*|103-*) echo "build/linux/$TREE/apps/testDescription" ;;
    104-*|105-*|106-*|107-*|108-*|109-*|111-*|112-*|113-*|114-*|115-*|116-*|117-*) echo "build/linux/$TREE/apps/testHandles" ;;
    110-*) echo "build/linux/$TREE/apps/testHost_callbacks" ;;
    118-*|119-*|120-*|121-*|122-*|123-*|124-*|125-*|126-*|127-*|128-*|129-*|130-*|131-*|132-*|134-*|135-*|136-*) echo "build/linux/$TREE/apps/testHost_context" ;;
    137-*|138-*|139-*|140-*|141-*|142-*|143-*|144-*|145-*|146-*|147-*|148-*|149-*|150-*|151-*) echo "build/linux/$TREE/apps/testCustom" ;;
    232-*|233-*|234-*|235-*|236-*|237-*|238-*|239-*|240-*|241-*|242-*|243-*|244-*|245-*|246-*|247-*|248-*|249-*|250-*|251-*|252-*|253-*|254-*|255-*|256-*|257-*|258-*|259-*|260-*|261-*|262-*|263-*|264-*|265-*|266-*|267-*|269-*|270-*|271-*|272-*|273-*|274-*|275-*|276-*|277-*) echo "build/linux/$TREE/apps/testCustom_ops" ;;
    300-*|301-*|302-*|303-*|304-*|305-*|306-*|307-*|308-*|309-*|310-*|311-*|312-*|313-*|314-*|315-*|316-*|317-*|318-*|319-*|320-*|321-*|322-*|323-*|324-*|325-*|326-*|327-*|328-*|329-*|330-*|331-*|333-*|334-*|335-*) echo "build/linux/$TREE/apps/testCustom_reentry" ;;
    332-*) echo "build/linux/$TREE/apps/testNative_calls" ;;
    152-*|153-*|154-*|155-*|157-*|158-*|159-*|160-*|161-*|162-*|163-*|164-*|165-*|166-*|167-*|168-*|169-*|170-*|171-*|172-*|173-*|174-*|175-*|176-*|179-*|180-*|181-*|182-*|183-*|184-*|185-*|186-*|187-*|188-*|189-*|190-*|191-*|198-*|199-*|202-*|203-*|204-*|205-*|206-*|207-*|208-*|209-*|210-*|211-*|212-*|213-*|214-*|215-*|216-*|217-*|218-*|222-*|223-*|225-*|226-*|227-*|228-*|229-*|230-*|231-*) echo "build/linux/$TREE/apps/testJit_templates" ;;
    177-*|200-*) echo "build/linux/$TREE/apps/testJit_floats" ;;
    156-*) echo "build/linux/$TREE/apps/testObserver" ;;
    06-*|07-*) echo "build/linux/$TREE/apps/oracle/oracle_ctang build/linux/$TREE/apps/testOracle" ;;
    13-*|14-*) echo "build/linux/$TREE/apps/testExecute_simple" ;;
    selftest) echo "build/linux/$TREE/apps/testObserver" ;;
  esac
}

run_test() {
  case "$1" in
    01-*|02-*)
      # The deep-print test deliberately skips under torture because building
      # its million-allocation nest would collect at every allocation. Exclude
      # only that named test; every selected control must still run and pass.
      (cd "$WORK" && env GRHEAP_TORTURE=1 GRHEAP_VERIFY=1 LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testExecute_complex --gtest_brief=1 --gtest_filter='*-Print.ADeeplyNestedContainerIsPrintedOnAThreadWithASmallStack') ;;
    03-*|05-*|selftest)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testObserver --gtest_brief=1) ;;
    08-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testObserver --gtest_brief=1 --gtest_filter='Observer.PlainTorture*') ;;
    09-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit --gtest_brief=1 --gtest_filter='Jit.FuelIsTheSame*') ;;
    32-*|33-*|34-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testObserver --gtest_brief=1 --gtest_filter='Observer.TheFloatPrograms*') ;;
    35-*|37-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_floats --gtest_brief=1 --gtest_filter='JitFloats.ARebuildUnderTorture*') ;;
    44-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_floats --gtest_brief=1 --gtest_filter='JitFloats.ThePoolIsLetGo*') ;;
    45-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_floats --gtest_brief=1 --gtest_filter='JitFloats.AFailedStackPush*') ;;
    47-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_floats --gtest_brief=1 --gtest_filter='JitFloats.AFailedRebuild*') ;;
    66-*|67-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_floats --gtest_brief=1 --gtest_filter='JitFloats.ACompiledCallWhoseCallee*') ;;
    46-*)
      # shellcheck disable=SC2086
      (cd "$WORK" && env -u GRHEAP_TORTURE -u GRHEAP_RELOCATE $RELOC_ENV LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_floats --gtest_brief=1 --gtest_filter='JitFloats.ARebuildUnderTorture*') ;;
    36-*|43-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_floats --gtest_brief=1 --gtest_filter='JitFloats.APauseAtAPoll*') ;;
    38-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_floats --gtest_brief=1 --gtest_filter='JitFloats.ReverseFits*') ;;
    39-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_floats --gtest_brief=1 --gtest_filter='JitFloats.AGuardThatFails*:JitFloats.ManyCompiledCalls*:JitFloats.ANestedEntry*') ;;
    40-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_floats --gtest_brief=1 --gtest_filter='JitFloats.AnUnwindWithFloatsLive*') ;;
    41-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_floats --gtest_brief=1 --gtest_filter='JitFloats.TheSlopeIsStillZeroWithAHandler*') ;;
    42-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_floats --gtest_brief=1 --gtest_filter='JitFloats.AMergeOfUnequalKinds*') ;;
    48-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_arith --gtest_brief=1 --gtest_filter='JitArith.EveryCaseTheInterpreterTurnsIntoAnError*') ;;
    49-*|55-*|58-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_arith --gtest_brief=1 --gtest_filter='JitArith.MixedIntegerAndFloat*') ;;
    79-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_arith --gtest_brief=1 --gtest_filter='JitArith.AFunctionWithAFloatPathAndANormalIntegerReturn*') ;;
    80-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_floats --gtest_brief=1 --gtest_filter='JitFloats.AFunctionDiscardedWhileItsRecordIsOpen*') ;;
    83-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_calls --gtest_brief=1 --gtest_filter='JitCalls.ACompiledCallLooksNoFunctionUp*') ;;

    73-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_arith --gtest_brief=1 --gtest_filter='JitArith.AFunctionWithALoopThatAlwaysEnds*') ;;
    74-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_arith --gtest_brief=1 --gtest_filter='JitArith.AFunctionWithALoopThatAlwaysEnds*') ;;
    75-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_arith --gtest_brief=1 --gtest_filter='JitArith.AFunctionWithARealExitOnOnePath*') ;;
    50-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_arith --gtest_brief=1 --gtest_filter='JitArith.AFloatReadFromAValueWord*:JitArith.EveryCaseTheInterpreterTurnsIntoAnError*') ;;
    51-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_arith --gtest_brief=1 --gtest_filter='JitArith.ANaNHeldRaw*') ;;
    52-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_arith --gtest_brief=1 --gtest_filter='JitArith.ACastToIntOfADoubleInside*') ;;
    53-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_arith --gtest_brief=1 --gtest_filter='JitArith.IntegerDivisionAndModulo*:JitArith.AnIntegerResultOutside*') ;;
    54-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_arith --gtest_brief=1 --gtest_filter='JitArith.AGuardThatFailsInTheMiddle*') ;;
    56-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_arith --gtest_brief=1 --gtest_filter='JitArith.FuelAndPolls*') ;;
    57-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_arith --gtest_brief=1 --gtest_filter='JitArith.ALoopOfPlusMinusAndTimes*:JitArith.TheSlopeIsStillZero*') ;;
    59-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_arith --gtest_brief=1 --gtest_filter='JitArith.UnaryMinusOfARawFloat*') ;;
    60-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_arith --gtest_brief=1 --gtest_filter='JitArith.AFloatReadFromAValueWord*:JitArith.EveryCaseTheInterpreterTurnsIntoAnError*') ;;
    61-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_arith --gtest_brief=1 --gtest_filter='JitArith.ACastToIntOfADoubleInside*') ;;
    62-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_arith --gtest_brief=1 --gtest_filter='JitArith.ADivisionByNaNOrInfinity*:JitArith.EveryOperationOverARawOperand*') ;;
    63-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_arith --gtest_brief=1 --gtest_filter='JitArith.IntegerDivisionAndModulo*:JitArith.EveryCaseTheInterpreterTurnsIntoAnError*') ;;
    64-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_arith --gtest_brief=1 --gtest_filter='JitArith.FuelAndPolls*:JitArith.ACastToInt*:JitArith.IntegerDivisionAndModulo*:JitArith.UnaryMinusOfARawFloat*:JitArith.ANaNHeldRaw*') ;;
    16-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_calls --gtest_brief=1 --gtest_filter='JitCalls.AFuelBudget*:JitCalls.ADeclaredGlobalFunction*') ;;
    17-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_calls --gtest_brief=1 --gtest_filter='JitCalls.RecursionPastTheGuestDepth*') ;;
    18-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testObserver --gtest_brief=1 --gtest_filter='Observer.PlainTorture*') ;;
    19-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_calls --gtest_brief=1 --gtest_filter='JitCalls.ARefusedRebuildAtTheEntry*') ;;
    21-*|22-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_calls --gtest_brief=1 --gtest_filter='JitCalls.ACalleeThatCannotBeCompiled*') ;;
    23-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_calls --gtest_brief=1 --gtest_filter='JitCalls.EveryParameterCountIsEntered*') ;;
    24-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_calls --gtest_brief=1 --gtest_filter='JitCalls.TheEighthExit*') ;;
    15-*|20-*)
      # shellcheck disable=SC2086
      (cd "$WORK" && env -u GRHEAP_TORTURE -u GRHEAP_RELOCATE $RELOC_ENV LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_calls --gtest_brief=1 --gtest_filter='JitCalls.ReferencesInPaddedFrames*:JitCalls.AChainOfSixty*:JitCalls.AChainOfFiveThousand*:JitCalls.AFramePushedForACallee*') ;;
    25-*|26-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testNative_calls --gtest_brief=1 --gtest_filter='NativeCalls.ANativeRunsInside*:NativeCalls.ALoopOfLibraryCalls*:NativeCalls.ABudgetOfNativeDepth*:NativeCalls.ANativeDepthBudgetSwept*') ;;
    27-*)
      # shellcheck disable=SC2086
      (cd "$WORK" && env -u GRHEAP_TORTURE -u GRHEAP_RELOCATE $RELOC_ENV LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testNative_calls --gtest_brief=1 --gtest_filter='NativeCalls.ReferencesPassedToANative*:NativeCalls.AChainOfSixty*') ;;
    28-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testNative_calls --gtest_brief=1 --gtest_filter='NativeCalls.EveryCompiledFrameBelowANestedActivation*') ;;
    29-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testNative_calls --gtest_brief=1 --gtest_filter='NativeCalls.ANativeWhoseResultUnwinds*') ;;
    31-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_calls --gtest_brief=1 --gtest_filter='JitCalls.ALongCompiledRunOfFib*') ;;
    30-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testNative_calls --gtest_brief=1 --gtest_filter='NativeCalls.ADottedUsePath*') ;;
    65-*)
      # The fresh-engine selection and the steady-state selection both check the answer; the plant is
      # caught only when both abort (and the control passes only when both pass).
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/bench/bench --smoke --only jit-float-loop-ret >/dev/null 2>&1); ra=$?
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/bench/bench --smoke --only steady-float-loop-ret >/dev/null 2>&1); rb=$?
      printf 'fresh selection exit %s, steady selection exit %s\n' "$ra" "$rb"
      if [ "$ra" -ne 0 ] && [ "$rb" -ne 0 ]; then exit 134; fi
      if [ "$ra" -eq 0 ] && [ "$rb" -eq 0 ]; then exit 0; fi
      exit 0 ;;
    68-*|69-*|70-*|71-*|72-*|76-*|77-*|78-*|81-*|82-*|192-*|193-*|194-*|195-*|196-*|197-*)
      sel=jit-intdiv-div3; case "$1" in 69-*) sel=steady-float-cmp ;; 70-*) sel=float-entry-cast-L1 ;; 71-*) sel=jit-float-cmp ;; 72-*) sel=steady-float-exit-str ;; 76-*) sel=steady-float-return-L1 ;; 77-*) sel=float-return-L0 ;; 78-*) sel=steady-float-entry-int-L1 ;; 81-*) sel=float-caller-declined ;; 82-*) sel=float-arg-L0 ;; 192-*) sel=steady-template-compiled-warm-call ;; 193-*|194-*|195-*) sel=steady-template-rows-compiled-per-row ;; 196-*) sel=steady-template-float-print-compiled-warm-call ;; 197-*) sel=steady-template-float-label-compiled-warm-call ;; esac
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/bench/bench --smoke --only "$sel") ;;
    11-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testSnapshot --gtest_brief=1 --gtest_filter='Snapshot.NoHostAddress*') ;;
    12-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testSnapshot --gtest_brief=1 --gtest_filter='Snapshot.OutputSoFar*:SnapshotCorpus.EveryProgram*') ;;
    04-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testNative_gate --gtest_brief=1) ;;
    84-*|85-*)
      (cd "$WORK" && env GRHEAP_TORTURE=1 GRHEAP_VERIFY=1 LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testHost_values --gtest_brief=1 --gtest_filter='HostValues.ANested*:HostValues.TheTagsAreRead*:HostValues.Numbers*') ;;
    86-*|87-*|88-*|89-*|90-*|95-*|98-*|99-*|101-*|102-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testHost_values --gtest_brief=1) ;;
    104-*|105-*|106-*|107-*|108-*|109-*|111-*|112-*|113-*|114-*|115-*|116-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testHandles --gtest_brief=1) ;;
    110-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testHost_callbacks --gtest_brief=1) ;;
    118-*|119-*|120-*|121-*|122-*|123-*|124-*|125-*|126-*|127-*|128-*|129-*|130-*|131-*|132-*|134-*|135-*|136-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testHost_context --gtest_brief=1) ;;
    137-*|138-*|139-*|140-*|141-*|142-*|143-*|144-*|145-*|146-*|147-*|148-*|149-*|150-*|151-*)
      # The collector's instruments are forced on inside the tests that need them.
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testCustom --gtest_brief=1) ;;
    232-*|233-*|234-*|235-*|236-*|237-*|238-*|239-*|240-*|241-*|242-*|243-*|244-*|245-*|246-*|247-*|248-*|249-*|250-*|251-*|252-*|253-*|254-*|255-*|256-*|257-*|258-*|259-*|260-*|261-*|262-*|263-*|264-*|265-*|266-*|267-*|269-*|270-*|271-*|272-*|273-*|274-*|275-*|276-*|277-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testCustom_ops --gtest_brief=1) ;;
    300-*|301-*|302-*|303-*|304-*|305-*|306-*|307-*|308-*|309-*|310-*|311-*|312-*|313-*|314-*|315-*|316-*|317-*|318-*|319-*|320-*|321-*|322-*|323-*|324-*|325-*|326-*|327-*|328-*|329-*|330-*|331-*|333-*|334-*|335-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testCustom_reentry --gtest_brief=1) ;;
    332-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testNative_calls --gtest_brief=1 --gtest_filter='NativeCalls.AResumableNativeIsReachedOnlyThroughAnExit*') ;;
    152-*|153-*|154-*|155-*|157-*|158-*|159-*|160-*|161-*|162-*|163-*|164-*|165-*|166-*|167-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_templates --gtest_brief=1) ;;
    169-*|176-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_templates --gtest_brief=1 --gtest_filter='JitTemplates.AHostDefinedOperand*') ;;
    170-*|173-*|174-*|175-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_templates --gtest_brief=1 --gtest_filter='JitTemplates.ALongConcatenation*') ;;
    171-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_templates --gtest_brief=1 --gtest_filter='JitTemplates.AConcatenationInALoop*') ;;
    172-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_templates --gtest_brief=1 --gtest_filter='JitTemplates.ARawFloatBeside*') ;;
    225-*|227-*|229-*|230-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_templates --gtest_brief=1 --gtest_filter='JitTemplates.ARawFloatsPrintConcatenation*') ;;
    226-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_templates --gtest_brief=1 --gtest_filter='JitTemplates.ARawFloatPrintedToTheHostsSink*') ;;
    228-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_templates --gtest_brief=1 --gtest_filter='JitTemplates.APrintedSumOfARawFloat*') ;;
    231-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_templates --gtest_brief=1 --gtest_filter='JitTemplates.AStringTheScanCannotSee*') ;;
    177-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_floats --gtest_brief=1 --gtest_filter='JitFloats.EveryOpcodeIsInBoth*') ;;
    179-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_templates --gtest_brief=1 --gtest_filter='JitTemplates.ALiteralsElementsSurvive*') ;;
    180-*|188-*|189-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_templates --gtest_brief=1 --gtest_filter='JitTemplates.ACastToIntOfAFloatConstant*') ;;
    187-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_templates --gtest_brief=1 --gtest_filter='JitTemplates.ATemplateOfCastsLiterals*') ;;
    190-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_templates --gtest_brief=1 --gtest_filter='JitTemplates.ALiteralWhoseAllocation*') ;;
    191-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_templates --gtest_brief=1 --gtest_filter='JitTemplates.AMapLiteralWhoseKeyAlone*') ;;
    181-*|183-*|184-*|185-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_templates --gtest_brief=1 --gtest_filter='JitTemplates.ACastToStringOfALargeArray*') ;;
    182-*|186-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_templates --gtest_brief=1 --gtest_filter='JitTemplates.LiteralsInALoop*') ;;
    168-*)
      # shellcheck disable=SC2086
      (cd "$WORK" && env -u GRHEAP_TORTURE -u GRHEAP_RELOCATE $RELOC_ENV LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_templates --gtest_brief=1 --gtest_filter='JitTemplates.ATemplateOfAttributes*:JitTemplates.AConcatenationInALoop*:JitTemplates.ValueOperationErrors*') ;;
    156-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testObserver --gtest_brief=1 --gtest_filter='Observer.ACompiledPrint*') ;;
    198-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_templates --gtest_brief=1 --gtest_filter='JitTemplates.APollInsideALongOperation*') ;;
    199-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_templates --gtest_brief=1 --gtest_filter='JitTemplates.ALargePrint*') ;;
    202-*|203-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_templates --gtest_brief=1 --gtest_filter='JitTemplates.AStandingOperationThatDidNotPoll*') ;;
    204-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_templates --gtest_brief=1 --gtest_filter='JitTemplates.AScopeThatRunsOutInsideAStanding*') ;;
    205-*|214-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_templates --gtest_brief=1 --gtest_filter='JitTemplates.TextsAndAShortStringAfterACallsFirstPrint*') ;;
    206-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_templates --gtest_brief=1 --gtest_filter='JitTemplates.ATextTheHostsSinkIsGiven*') ;;
    207-*|212-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_templates --gtest_brief=1 --gtest_filter='JitTemplates.ATextThatDoesNotFitTheOutput*') ;;
    208-*|210-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_templates --gtest_brief=1 --gtest_filter='JitTemplates.ATextAfterAnotherTag*') ;;
    209-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_templates --gtest_brief=1 --gtest_filter='JitTemplates.APrintThatFailsAfterFastTexts*') ;;
    211-*|215-*|216-*|217-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_templates --gtest_brief=1 --gtest_filter='JitTemplates.AMapsAttributeIsTheFastPath*') ;;
    213-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_templates --gtest_brief=1 --gtest_filter='JitTemplates.AStringPrintedWhereItsTemporaryCannotBeHeld*') ;;
    218-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_templates --gtest_brief=1 --gtest_filter='JitTemplates.AMapsAttributeInATemplateReadsTheTemplatesOwnConstants*') ;;
    221-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_calls --gtest_brief=1 --gtest_filter='JitCalls.ATinyNativeStack*') ;;
    222-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_templates --gtest_brief=1 --gtest_filter='JitTemplates.ATextAfterAnotherTag*') ;;
    223-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_templates --gtest_brief=1) ;;
    224-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testNative_calls --gtest_brief=1 --gtest_filter='NativeCalls.ANativeOfEveryArity*') ;;
    219-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testEngine --gtest_brief=1 --gtest_filter='ErrorOrigin.AnOperatorOfTwoIntegersThatTheInterpreterCalls*') ;;
    220-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testNative_calls --gtest_brief=1 --gtest_filter='NativeCalls.ALoopOfLibraryCallsInACompiledFunctionMakesNoExit') ;;
    200-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit_floats --gtest_brief=1 --gtest_filter='JitFloats.ManyCompiledCalls*:JitFloats.ANestedEntry*') ;;
    201-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit --gtest_brief=1 --gtest_filter='Jit.FuelIsTheSame*') ;;
    117-*)
      # shellcheck disable=SC2086
      (cd "$WORK" && env -u GRHEAP_TORTURE -u GRHEAP_RELOCATE $RELOC_ENV LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testHandles --gtest_brief=1) ;;
    91-*|92-*|93-*|94-*|96-*|97-*|100-*|103-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testDescription --gtest_brief=1) ;;
    13-*|14-*)
      # shellcheck disable=SC2086
      (cd "$WORK" && env -u GRHEAP_TORTURE -u GRHEAP_RELOCATE $RELOC_ENV LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testExecute_simple --gtest_brief=1) ;;
    06-*|07-*)
      (cd "$WORK" && env GLTANG_ORACLE_RUNNER="$APPS/oracle/oracle_ctang" LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testOracle --gtest_brief=1) ;;
  esac
}

# A passing control must have executed and passed all selected Google Test
# cases. The
# benchmark-smoke plants are the only non-GTest run_test branches.
completed_selected_cases() {
  case "$1" in
    65-*|68-*|69-*|70-*|71-*|72-*|76-*|77-*|78-*|81-*|82-*|192-*|193-*|194-*|195-*|196-*|197-*) return 0 ;;
  esac
  printf '%s\n' "$2" | awk '
    /^\[=+\] [1-9][0-9]* tests? from [1-9][0-9]* test suites? ran\./ {
      completed = $2; completed_lines++
    }
    /^\[[[:space:]]*PASSED[[:space:]]*\] [1-9][0-9]* tests?\./ {
      passed = $4; passed_lines++
    }
    /^\[[[:space:]]*SKIPPED[[:space:]]*\]/ { skipped_lines++ }
    END {
      exit !(completed_lines == 1 && passed_lines == 1 &&
             completed > 0 && completed == passed && skipped_lines == 0)
    }
  '
}

if [ "$SELECTION_SELFTEST" -eq 1 ]; then
  good='[==========] 2 tests from 1 test suite ran.
[  PASSED  ] 2 tests.'
  zero='[==========] 0 tests from 0 test suites ran.
[  PASSED  ] 0 tests.'
  all_skipped='[==========] 2 tests from 1 test suite ran.
[  SKIPPED ] 2 tests, listed below:'
  partial_skipped='[==========] 2 tests from 1 test suite ran.
[  PASSED  ] 1 test.
[  SKIPPED ] 1 test, listed below:'
  if ! completed_selected_cases selftest "$good" ||
     completed_selected_cases selftest "$zero" ||
     completed_selected_cases selftest "$all_skipped" ||
     completed_selected_cases selftest "$partial_skipped"; then
    printf 'check-planted: selection selftest failed\n' >&2
    exit 1
  fi
  printf 'check-planted: selection selftest passed\n'
  exit 0
fi

name_of_test() {
  case "$1" in
    01-*|02-*) echo "testExecute_complex under torture and verify" ;;
    03-*|05-*|selftest) echo "testObserver" ;;
    08-*) echo "testObserver (the frame differential, interpreter against JIT)" ;;
    32-*|33-*|34-*) echo "testObserver (the float programs, compared by bits, interpreter against JIT)" ;;
    09-*) echo "testJit (the fuel-parity test)" ;;
    15-*|20-*) echo "testJit_calls under relocation torture" ;;
    35-*|37-*) echo "testJit_floats (the rebuild under torture and verify)" ;;
    44-*) echo "testJit_floats (the pool test: empty at a pause, and the snapshot restores)" ;;
    45-*) echo "testJit_floats (the failed stack push gives its extension back)" ;;
    46-*) echo "testJit_floats under relocation torture" ;;
    66-*|67-*) echo "testJit_floats (the float-free call test: no lookup and no record across fib(15))" ;;
    47-*) echo "testJit_floats (the failed rebuild under torture and verify)" ;;
    36-*|43-*) echo "testJit_floats (the pause with live raw values, read back by their bits)" ;;
    38-*) echo "testJit_floats (the checked write-back over a hand-made F64 site)" ;;
    39-*) echo "testJit_floats (the reservation's capacity across returns, deopts and nested entries)" ;;
    40-*) echo "testJit_floats (the unwind with floats live)" ;;
    41-*) echo "testJit_floats (the allocation slope with a handler pending)" ;;
    42-*) echo "testJit_floats (the merge programs, interpreter against JIT)" ;;
    48-*) echo "testJit_arith (the error rows: every case the interpreter turns into an error)" ;;
    49-*|55-*|58-*) echo "testJit_arith (the mixed integer and float rows)" ;;
    79-*) echo "testJit_arith (the float path beside a normal integer return must compile)" ;;
    80-*) echo "testJit_floats (a function discarded while its record is open)" ;;
    73-*|74-*) echo "testJit_arith (the looped float-returner that must stay compiled and entered)" ;;
    75-*) echo "testJit_arith (the function with a real exit path beside a float return)" ;;
    50-*) echo "testJit_arith (the string, array and boolean operand rows)" ;;
    51-*) echo "testJit_arith (the NaN comparison rows)" ;;
    52-*) echo "testJit_arith (the cast rows)" ;;
    53-*) echo "testJit_arith (the integer division rows, SMALL_MIN / -1)" ;;
    54-*) echo "testJit_arith (the guard that fails in the middle of an operation)" ;;
    56-*) echo "testJit_arith (the fuel-parity test)" ;;
    57-*) echo "testJit_arith (the allocation slope and the exit counts)" ;;
    59-*) echo "testJit_arith (the -0.0 row)" ;;
    60-*) echo "testJit_arith (the null, boolean and error operand rows; a wild load, so the process is killed)" ;;
    61-*) echo "testJit_arith (the cast rows, the lower bound)" ;;
    62-*) echo "testJit_arith (the division by NaN row)" ;;
    63-*) echo "testJit_arith (the modulo rows)" ;;
    64-*) echo "testJit_arith (the fuel comparison of the cast, negation, comparison, modulo and division rows)" ;;
    65-*) echo "bench --smoke (the answer check of the float loop, fresh and steady selections)" ;;
    68-*) echo "bench --smoke (the integer-reference path of the answer check)" ;;
    69-*) echo "bench --smoke (the steady-state answer check)" ;;
    70-*) echo "bench --smoke (the family cases' exact expectation)" ;;
    71-*) echo "bench --smoke (a compiled row must have compiled and entered)" ;;
    72-*) echo "bench --smoke (the number of functions discarded)" ;;
    76-*) echo "bench --smoke (the number of exits that only box a raw float)" ;;
    77-*) echo "bench --smoke (a loop-free float-returning function must be seen declined)" ;;
    78-*) echo "bench --smoke (the number of exits that count)" ;;
    81-*) echo "bench --smoke (the exits are of exactly one kind)" ;;
    82-*) echo "bench --smoke (a declined function is not entered; its callee is, exactly)" ;;
    192-*) echo "bench --smoke (the template policy of the warm template: no exit, never discarded)" ;;
    193-*) echo "bench --smoke (the template policy of the per-row template: no designed exit, every compiled call returns)" ;;
    194-*) echo "bench --smoke (the template policy: compiled entries returned exactly)" ;;
    195-*) echo "bench --smoke (the template policy: entries into compiled code exactly)" ;;
    196-*) echo "bench --smoke (the float-print template's policy: no exit, every compiled call returns)" ;;
    197-*) echo "bench --smoke (the float-label template's policy: no exit, every compiled call returns)" ;;
    83-*) echo "testJit_calls (the lookup-count test: the same lookups for fib(8) and fib(20))" ;;
    16-*) echo "testJit_calls (the fuel-parity tests)" ;;
    17-*) echo "testJit_calls (the recursion-limit test)" ;;
    18-*) echo "testObserver (the frame differential, a caller's line after a chain is rebuilt)" ;;
    19-*) echo "testJit_calls (the injected rebuild failure)" ;;
    21-*) echo "testJit_calls (the remembered-exit count)" ;;
    22-*) echo "testJit_calls (the uncompilable-callee test)" ;;
    23-*) echo "testJit_calls (the entry-form parity test)" ;;
    24-*) echo "testJit_calls (the discard test)" ;;
    25-*|26-*) echo "testNative_calls (the native depth a native sees, the loop of library calls)" ;;
    27-*) echo "testNative_calls under relocation torture" ;;
    28-*) echo "testNative_calls (the nested activation on a stack that moves)" ;;
    29-*) echo "testNative_calls (the unwind inside a native's answer)" ;;
    30-*) echo "testNative_calls (the loads of a dotted path and of a name)" ;;
    31-*) echo "testJit_calls (the long compiled run of fib, zero call exits)" ;;
    04-*) echo "testNative_gate" ;;
    84-*|85-*) echo "testHost_values under torture and verify" ;;
    86-*) echo "testHost_values (the rendering differentials)" ;;
    87-*) echo "testHost_values (the map-key test)" ;;
    88-*) echo "testHost_values (the fuel a long copy is charged)" ;;
    89-*) echo "testHost_values (the memory-budget test of a factory)" ;;
    90-*) echo "testHost_values (the over-deep description, through the test hook)" ;;
    95-*) echo "testHost_values (the fresh-copy tests)" ;;
    98-*) echo "testHost_values (the description past its own node limit)" ;;
    99-*) echo "testHost_values (the description past its own byte limit)" ;;
    101-*|102-*) echo "testHost_values (the description that claims no limit, against the lowered ceilings)" ;;
    91-*) echo "testDescription (the depth-limit tests)" ;;
    92-*) echo "testDescription (the node-limit test)" ;;
    93-*) echo "testDescription (the byte-limit tests)" ;;
    94-*) echo "testDescription (the one-parent tests)" ;;
    96-*|97-*) echo "testDescription (the cycle tests)" ;;
    100-*) echo "testDescription (the invalid-text tests)" ;;
    103-*) echo "testDescription (the foreign-ref test)" ;;
    104-*) echo "testHandles (the persistent handle across collections under torture)" ;;
    105-*|116-*) echo "testHandles (the stale-handle test)" ;;
    106-*) echo "testHandles (the scope-limit test)" ;;
    107-*) echo "testHandles (the segment test)" ;;
    108-*|112-*) echo "testHandles (the context's leak check)" ;;
    111-*) echo "testHandles (the count of cutil allocations across a run)" ;;
    109-*) echo "testHandles (the persistent-limit test)" ;;
    110-*) echo "testHost_callbacks (the label gate)" ;;
    118-*|119-*|120-*|121-*|122-*|123-*|124-*|125-*|126-*|127-*|128-*|129-*|130-*|131-*|132-*|134-*|135-*|136-*) echo "testHost_context" ;;
    137-*|139-*|141-*) echo "testCustom (the reflection rule: the log of calls, the decline allocation count)" ;;
    138-*|140-*) echo "testCustom (the reflected and built-in-on-the-left answers)" ;;
    142-*|143-*) echo "testCustom (the slots under forced torture and verify)" ;;
    144-*|145-*|146-*|147-*) echo "testCustom (the registry: attach, seal, binding, identity)" ;;
    148-*|149-*) echo "testCustom (the equality table, the host-function test)" ;;
    150-*|151-*) echo "testCustom (the error-operand and container-comparison error tests)" ;;
    232-*|233-*|234-*|235-*|236-*|237-*|238-*|239-*|240-*|241-*|242-*|243-*|244-*|245-*|246-*|247-*|248-*|249-*|250-*|251-*|252-*|253-*|254-*|255-*|256-*|257-*|258-*|259-*|260-*|261-*|262-*|263-*|264-*|265-*|266-*|267-*|269-*|270-*|271-*|272-*|273-*|274-*|275-*|276-*|277-*) echo "testCustom_ops (story 6: iteration, calls, casts, truthiness, print and to-string, the error mark, attributes of built-in kinds)" ;;
    300-*|301-*|302-*|303-*|304-*|305-*|306-*|307-*|308-*|309-*|310-*|311-*|312-*|313-*|314-*|315-*|316-*|317-*|318-*|319-*|320-*|321-*|322-*|323-*|324-*|325-*|326-*|327-*|328-*|329-*|330-*|331-*|333-*|334-*|335-*) echo "testCustom_reentry (story 9: a host callback that calls guest code)" ;;
    332-*) echo "testNative_calls (a resumable native is reached only through an exit)" ;;
    152-*|153-*) echo "testJit_templates (the loop of prints: fuel and the print's value)" ;;
    154-*) echo "testJit_templates (the large-print test: pacing polls, fuel and the scope that runs out inside it)" ;;
    155-*) echo "testJit_templates (the refused delivery: the error's origin is the print's line; the large print: the identity of every poll)" ;;
    156-*) echo "testObserver (the frame differential over the output programs)" ;;
    157-*) echo "testJit_templates (the large-print test: the identity of every poll)" ;;
    158-*) echo "testJit_templates (the template whose first operation is text: its fuel)" ;;
    159-*) echo "testJit_templates (the warm template, and every test whose fuel is compared with the interpreter's)" ;;
    160-*) echo "testJit_templates (the warm template: its counted exits, and that it is still compiled)" ;;
    161-*) echo "testJit_templates (the error result a later statement replaces: its discarded record)" ;;
    162-*) echo "testJit_templates (the top-level variables, read back)" ;;
    163-*) echo "testJit_templates (the bare statement whose value is an error: its three entries)" ;;
    164-*) echo "testJit_templates (the bare statement whose value is an error: the position its run ends with)" ;;
    165-*) echo "testJit_templates (the raw-float box exits, one scenario per operation)" ;;
    166-*) echo "testJit_templates (the error result a variable holds: not lost, not recorded)" ;;
    167-*) echo "testJit_templates (the template call that ends a run: the position it ends with)" ;;
    168-*) echo "testJit_templates under relocation torture" ;;
    169-*|176-*) echo "testJit_templates (the host-defined operand of each value operation: its exits and helper calls)" ;;
    170-*|173-*|174-*|175-*) echo "testJit_templates (the long operations: the identity of every poll)" ;;
    177-*) echo "testJit_floats (the opcode policy over both kind tables)" ;;
    198-*) echo "testJit_templates (the poll identities located to their lines, each tier on its own)" ;;
    199-*) echo "testJit_templates (the large-print test's template ending in a slow text: the identity of every poll)" ;;
    202-*|203-*) echo "testJit_templates (the memory sweep after a short standing operation: the identity of every poll)" ;;
    204-*) echo "testJit_templates (the scope that runs out inside an attribute's name: the identity of every poll)" ;;
    205-*) echo "testJit_templates (the template of texts and a short string: its fuel)" ;;
    206-*) echo "testJit_templates (the loop of texts with a host sink: its deliveries)" ;;
    207-*) echo "testJit_templates (the strings at and below the pacing interval: their polls)" ;;
    208-*|210-*) echo "testJit_templates (the texts and HTML strings that alternate: the rendered output)" ;;
    209-*) echo "testJit_templates (the memory sweep over a long print refused after a fast one: the rollback)" ;;
    211-*) echo "testJit_templates (the loop of a map's attribute: the heap objects made)" ;;
    212-*) echo "testJit_templates (the sixteen-byte texts: the fast-path count the requirement gives)" ;;
    213-*) echo "testJit_templates (the memory sweep over a first print of a string)" ;;
    214-*) echo "testJit_templates (the template of texts and a short string: its fuel)" ;;
    215-*) echo "testJit_templates (the loop of a map's attribute: its fuel)" ;;
    216-*) echo "testJit_templates (the loop of a map's attribute beside a string's and a library's)" ;;
    217-*) echo "testJit_templates (the loop of a map's attribute: the fast path's hash assert)" ;;
    218-*) echo "testJit_templates (a template's m.name after the script made thirty constants: the heap objects)" ;;
    200-*) echo "testJit_floats (the reservation's capacity across compiled calls and a nested entry)" ;;
    219-*) echo "testEngine (the offset of a division's and a modulo's error, against the disassembly)" ;;
    220-*) echo "testNative_calls (the loop of library calls in a compiled function: no exit)" ;;
    221-*) echo "testJit_calls (the tiny native stack: its 16 KiB row enters a small function)" ;;
    222-*) echo "testJit_templates (the texts and HTML strings that alternate: the rendered output)" ;;
    223-*) echo "testJit_templates (every test: errors, polls and identities against the interpreter's)" ;;
    224-*) echo "testNative_calls (the natives of every arity called from compiled code)" ;;
    201-*) echo "testJit (the fuel-parity test)" ;;
    179-*) echo "testJit_templates (the literal of fresh strings under torture, verify and a moving stack)" ;;
    180-*) echo "testJit_templates (the corpus scripts of out-of-range constant casts)" ;;
    188-*|189-*) echo "testJit_templates (the constant casts at both bounds)" ;;
    181-*|183-*) echo "testJit_templates (the cast to string of a large array: the identity of every poll)" ;;
    184-*|185-*) echo "testJit_templates (the copying ADOPT of a large array: the identity of every poll)" ;;
    182-*) echo "testJit_templates (the literals in a loop: the held row)" ;;
    186-*) echo "testJit_templates (the literals in a loop: the held map)" ;;
    187-*) echo "testJit_templates (the template of casts and literals: its fuel)" ;;
    190-*) echo "testJit_templates (the memory sweep over a compiled function's literals)" ;;
    191-*) echo "testJit_templates (the memory sweep where only a long map key is refused)" ;;
    171-*) echo "testJit_templates (the concatenation in a loop: its fuel)" ;;
    172-*) echo "testJit_templates (the raw float beside a string: its expected and counted exits)" ;;
    225-*|230-*) echo "testJit_templates (the prints, concatenations and casts of the special values: fuel)" ;;
    227-*|229-*) echo "testJit_templates (the prints, concatenations and casts of the special values: their text)" ;;
    226-*) echo "testJit_templates (the raw float printed to the host's sink: its deliveries and output)" ;;
    228-*) echo "testJit_templates (the printed sum of a raw float and a value word: fuel)" ;;
    231-*) echo "testJit_templates (the string the scan cannot see: the merge's counted exits)" ;;
    113-*) echo "testHandles (the frame test)" ;;
    114-*) echo "testHandles (the answer tests)" ;;
    115-*) echo "testHandles (the copy test)" ;;
    117-*) echo "testHandles under relocation torture" ;;
    11-*) echo "testSnapshot (the address scan)" ;;
    12-*) echo "testSnapshot (the output-so-far test and the corpus sweep)" ;;
    13-*|14-*) echo "testExecute_simple under relocation torture" ;;
    06-*|07-*) echo "testOracle" ;;
  esac
}

build() {
  # Incremental: only what the patch touched is rebuilt. The environment's make
  # flags are not this make's.
  (cd "$WORK" && env -u MAKEFLAGS -u MFLAGS -u MAKELEVEL -u JIT make -j"$JOBS" JIT="$JIT" $PREFIX_ARG $(target_of "$1") >>"$LOG" 2>&1)
}

# The files a patch names, from its +++ lines.
files_of() {
  sed -n 's|^+++ b/\([^[:space:]]*\).*|\1|p' "$1"
}

# The line that shows why the test failed: the first assertion, abort or
# violation, else the last line it printed.
failing_line() {
  out="$1"
  line="$(printf '%s\n' "$out" | grep -m1 -A3 -E 'Failure|FAILED|Assertion|barrier|violation|Abort|ERROR|unreadable|too many|exception|Segmentation|signal' | grep -v -E '^(--|[[:space:]]*)$' | grep -v 'Which is' | head -3 | tr '\n' ' ' | cut -c1-230)"
  if [ -z "$line" ]; then
    line="$(printf '%s\n' "$out" | grep -v '^[[:space:]]*$' | tail -1 | cut -c1-230)"
  fi
  printf '%s' "$line"
}

# run_case <case> <patch file>; 0 caught with a passing control, 1 not caught or
# the control failed, 2 the patch did not apply.
run_case() {
  case_name="$1"
  patch_file="$2"
  printf 'planted %s\n' "$case_name"
  files="$(files_of "$patch_file")"
  if [ -z "$files" ]; then
    printf '  FAIL: the patch names no file\n' >&2
    return 2
  fi
  for f in $files; do
    if [ ! -f "$WORK/$f" ]; then
      printf '  FAIL: the patch names %s, which the copy does not have\n' "$f" >&2
      return 2
    fi
  done
  # Apply, and check it matched.
  if ! patch -p1 --forward --fuzz=0 -s -d "$WORK" -i "$patch_file" >/dev/null 2>&1; then
    printf '  FAIL: the patch did not apply (it matched nothing, or only part of it): %s\n' "$patch_file" >&2
    # Whatever it did apply is taken back out.
    for f in $files; do cp "$ROOT/$f" "$WORK/$f"; done
    return 2
  fi
  changed=0
  for f in $files; do
    if ! cmp -s "$ROOT/$f" "$WORK/$f"; then
      changed=1
    fi
  done
  if [ "$changed" = 0 ]; then
    printf '  FAIL: the patch applied and changed nothing\n' >&2
    return 2
  fi
  rc=0
  # The patched copy: build, run, and the run must fail.
  if ! build "$case_name"; then
    printf '  FAIL: the patched copy did not build (see %s); a defect that does not compile is no proof\n' "$LOG" >&2
    rc=1
  else
    out="$(run_test "$case_name" 2>&1)"
    status=$?
    if [ "$status" -eq 0 ]; then
      printf '  NOT CAUGHT: %s passed with the defect in place\n' "$(name_of_test "$case_name")" >&2
      rc=1
    elif [ "$status" -eq 124 ]; then
      printf '  NOT CAUGHT: %s timed out (exit 124); a hang is not a failure of the instrument\n' "$(name_of_test "$case_name")" >&2
      rc=1
    else
      printf '  caught by %s (exit %s): %s\n' "$(name_of_test "$case_name")" "$status" "$(failing_line "$out")"
      case "$case_name" in
        13-*|14-*|15-*|20-*|27-*|46-*|117-*|168-*)
          # Relocation is the instrument: with torture on and relocation off the
          # same patched build passes.
          saved="$RELOC_ENV"
          RELOC_ENV="GRHEAP_TORTURE=1"
          out="$(run_test "$case_name" 2>&1)"
          status=$?
          RELOC_ENV="$saved"
          if [ "$status" -ne 0 ]; then
            printf '  FAIL: with the defect in place and relocation off, %s still failed (exit %s), so relocation is not what found it: %s\n' "$(name_of_test "$case_name")" "$status" "$(failing_line "$out")" >&2
            rc=1
          elif ! completed_selected_cases "$case_name" "$out"; then
            printf '  FAIL: relocation-off control selected no completed Google Test cases\n' >&2
            rc=1
          else
            printf '  and passes with relocation off: relocation is the instrument\n'
          fi
          ;;
      esac
    fi
  fi
  # The control: the same copy with the patch taken out.
  for f in $files; do cp "$ROOT/$f" "$WORK/$f"; done
  if ! build "$case_name"; then
    printf '  FAIL: the control did not build (see %s)\n' "$LOG" >&2
    return 1
  fi
  out="$(run_test "$case_name" 2>&1)"
  status=$?
  if [ "$status" -ne 0 ]; then
    printf '  CONTROL FAILED: %s fails without the defect (exit %s): %s\n' "$(name_of_test "$case_name")" "$status" "$(failing_line "$out")" >&2
    rc=1
  elif ! completed_selected_cases "$case_name" "$out"; then
    printf '  CONTROL FAILED: %s selected no completed Google Test cases\n' "$(name_of_test "$case_name")" >&2
    rc=1
  else
    printf '  control passes\n'
  fi
  return $rc
}

make_copy() {
  rm -rf "$ROOT/build/planted"
  mkdir -p "$WORK"
  : >"$LOG"
  (cd "$ROOT" && tar --exclude=./build -cf - Makefile bison flex include src tests tools pkgconfig documentation examples bench 2>/dev/null) | tar -C "$WORK" -xf -
  if [ ! -f "$WORK/Makefile" ] || [ ! -d "$WORK/src" ]; then
    printf 'check-planted: could not make the copy under %s\n' "$WORK" >&2
    exit 2
  fi
}

failures=0

if [ "$SELFTEST" = 1 ]; then
  if ! "$0" --selftest-selection; then
    printf '  FAIL: selected-case pass/skip control selftest failed\n' >&2
    exit 1
  fi
  make_copy
  printf 'check-planted --selftest\n'
  # The unpatched copy must build for the rest to mean anything.
  if ! build selftest; then
    printf '  FAIL: the copy did not build (see %s)\n' "$LOG" >&2
    exit 1
  fi
  run_case selftest "$PATCHES/selftest/matches-nothing.patch" >/dev/null 2>&1
  rc=$?
  if [ "$rc" -eq 2 ]; then
    printf '  ok   a patch that applies to nothing fails the script\n'
  else
    printf '  FAIL: a patch that applies to nothing did not fail the script (rc %s)\n' "$rc" >&2
    failures=$((failures + 1))
  fi
  run_case selftest "$PATCHES/selftest/harmless.patch" >/dev/null 2>&1
  rc=$?
  if [ "$rc" -eq 1 ]; then
    printf '  ok   a patch that breaks nothing is reported as not caught\n'
  else
    printf '  FAIL: a harmless patch was not reported as not caught (rc %s)\n' "$rc" >&2
    failures=$((failures + 1))
  fi
  # JIT=no skips the cases whose defect lives in the JIT, says so, and does not
  # count a skip as a catch: a run that can only skip fails. (The nested run is
  # a fresh script; it makes its own copy and removes it, after this one is done
  # with its own.)
  skip_out="$(PLANTED_JIT=no "$0" 08-jit-wrong-tag 09-jit-skipped-fuel 2>&1)"
  skip_rc=$?
  case "$skip_out" in
    *"planted 08-jit-wrong-tag"*"SKIPPED"*"planted 09-jit-skipped-fuel"*"SKIPPED"*) skip_said=1 ;;
    *) skip_said=0 ;;
  esac
  if [ "$skip_rc" -eq 1 ] && [ "$skip_said" -eq 1 ]; then
    printf '  ok   under JIT=no the JIT cases are skipped out loud, and a run of nothing but skips fails\n'
  else
    printf '  FAIL: JIT=no did not skip the JIT cases visibly and fail an all-skipped run (rc %s): %s\n' "$skip_rc" "$skip_out" >&2
    failures=$((failures + 1))
  fi
  if [ "$failures" -ne 0 ]; then
    exit 1
  fi
  printf 'check-planted --selftest: the script fails on a patch that matches nothing and on one that breaks nothing\n'
  exit 0
fi

case "$MODE" in
  quick) CASES="$QUICK" ;;
  slow) CASES="$SLOW" ;;
  all) CASES="$SLOW $QUICK" ;;
  relocate) CASES="$RELOCATE_CASES" ;;
esac
if [ -n "$NAMED" ]; then
  CASES="$NAMED"
fi

make_copy
count=0
skipped=0
for c in $CASES; do
  patch_file="$PATCHES/$c.patch"
  if [ ! -f "$patch_file" ]; then
    printf 'check-planted: no patch %s\n' "$patch_file" >&2
    exit 2
  fi
  if [ "$JIT" = no ]; then
    case " $JIT_ONLY " in
      *" $c "*)
        printf 'planted %s\n  SKIPPED: JIT=no builds no JIT, so there is nothing to plant this defect in; it runs under JIT=yes\n' "$c"
        skipped=$((skipped + 1))
        continue
        ;;
    esac
  fi
  count=$((count + 1))
  if ! run_case "$c" "$patch_file"; then
    failures=$((failures + 1))
  fi
done

if [ "$count" -eq 0 ]; then
  printf 'check-planted: no case ran (%s skipped), so this measures nothing\n' "$skipped" >&2
  exit 1
fi
if [ "$failures" -ne 0 ]; then
  printf 'check-planted: %s of %s planted defects were not caught by their instrument, or their control failed (%s skipped)\n' "$failures" "$count" "$skipped" >&2
  exit 1
fi
printf 'check-planted: all %s planted defects were caught by the instrument named for them, and each control passed; %s skipped (JIT=%s)\n' "$count" "$skipped" "$JIT"
