#!/bin/sh
#
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2024-2026 Corey Pennycuff
#
# This file is part of Ghoti.io Lang-tang.
#
# Ghoti.io Lang-tang is free software: you can redistribute it and/or modify it
# under the terms of the GNU Lesser General Public License version 3 as
# published by the Free Software Foundation.
#
# Ghoti.io Lang-tang is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
# for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

# Exercises the command line utility and checks what it produces and how it
# exits. Adapted from ctang's cli-test.sh.
#
# The gtest suites link the library and never run the binary, so everything
# the binary does on its own - reading the file, reading stdin, reporting a
# file it cannot read, the exit status of each outcome - is checked here.
#
# Usage: cli-test.sh <path to tang>

set -u

TANG="${1:?usage: cli-test.sh <path to tang>}"
HERE="$(cd "$(dirname "$0")" && pwd)"
CORPUS="$HERE/corpus"
failures=0
TMPFILE="$(mktemp)"
trap 'rm -f "$TMPFILE"' EXIT

# check <name> <expected> <actual>
check() {
  if [ "$2" = "$3" ]; then
    printf '  ok    %s\n' "$1"
  else
    printf '  FAIL  %s\n        expected [%s]\n        got      [%s]\n' "$1" "$2" "$3"
    failures=$((failures + 1))
  fi
}

# status <name> <expected status> <command...>: runs it with all output
# discarded and checks the exit status only.
status() {
  name="$1"; want="$2"; shift 2
  "$@" >/dev/null 2>&1
  got=$?
  check "$name" "$want" "$got"
}

# The tree for 1 + 2, which every way of giving the source must produce.
TREE='Binary (+):
  LHS:
    Integer: 1
  RHS:
    Integer: 2'

# --tree keeps the old behaviour: parse, print the syntax tree, run nothing.
# A script and a template read from a file.
check "file, script" "$TREE" "$("$TANG" --tree -s "$CORPUS/script/expression-only.tang" | head -5)"
check "file, template: text becomes a print" \
  "Block:" "$("$TANG" --tree "$CORPUS/template/plain-text.tang" | head -1)"

# The source from the command line.
check "-e, script" "$TREE" "$("$TANG" --tree -s -e '1 + 2')"
check "--evaluate --script" "$TREE" "$("$TANG" --tree --script --evaluate '1 + 2')"
check "-e, template" "Block:" "$("$TANG" --tree -t -e 'Hello <%= 1+1 %>!' | head -1)"

# The same, read from stdin.
check "stdin, script" "$TREE" "$(printf '1 + 2' | "$TANG" --tree -s)"
check "stdin, template" "Block:" "$(printf 'Hello <%%= 1+1 %%>!' | "$TANG" --tree | head -1)"

# Empty stdin is an empty program, not a read of whatever followed the buffer
# in memory: no output and a success, run or dumped.
check "stdin, empty" "" "$(printf '' | "$TANG" --tree -s)"
status "stdin, empty exits 0" 0 sh -c "printf '' | '$TANG'"
check "stdin, empty runs to nothing" "" "$(printf '' | "$TANG" -s)"
check "-e '', empty" "" "$("$TANG" -s -e '')"

# Running. The output is written rendered: every piece of text encoded as its
# tag says. -e is code, so these are the programs the reference prints.
check "-e prints" "3" "$("$TANG" -e 'print(1+2);')"
check "-e renders per segment" '<b>&lt;i&gt;' "$("$TANG" -e 'print("<b>" + !"<i>");')"
check "-e -s is the same" "3" "$("$TANG" -s -e 'print(1+2);')"
check "-e -t is a template" "Hello 2!" "$("$TANG" -t -e 'Hello <%= 1+1 %>!')"
check "a file runs as a template" "Hello, world!" "$("$TANG" "$CORPUS/template/plain-text.tang" | head -1)"
check "a script file runs" "3" "$(printf 'print(1+2);' > "$TMPFILE"; "$TANG" -s "$TMPFILE")"
check "stdin runs as a template" "Hello 2!" "$(printf 'Hello <%%= 1+1 %%>!' | "$TANG")"
check "stdin runs as a script with -s" "3" "$(printf 'print(1+2);' | "$TANG" -s)"
check "the result is not printed" "" "$("$TANG" -e '1 + 2;')"
check "errors are values and print as nothing" "a" "$("$TANG" -e 'print("a"); print(1/0);')"
check "a marker prints as itself" "[INTEGER TOO LARGE]" "$("$TANG" -e 'print(9223372036854775807 + 1);')"
check "recursion past the depth yields the error and the program goes on" "after" \
  "$("$TANG" -e 'function f(n) { return f(n + 1); } f(0); print("after");')"
check "--depth moves the limit" "ok" \
  "$("$TANG" --depth 100 -e 'function d(n) { if (n <= 0) { return 0; } return d(n - 1); } if (d(99) == 0) { print("ok"); } else { print("no"); }')"
check "--depth bounds it" "no" \
  "$("$TANG" --depth 10 -e 'function d(n) { if (n <= 0) { return 0; } return d(n - 1); } if (d(20) == 0) { print("ok"); } else { print("no"); }')"
check "50,000 deep recursion, depth raised" "50000" \
  "$("$TANG" --depth 100000 -e 'function d(n) { if (n <= 0) { return 0; } return 1 + d(n - 1); } print(d(50000));')"

# A runaway loop under a fuel budget pauses; the command has no one to resume
# it, so it says where and exits 5. The process survives.
out="$("$TANG" --fuel 5000 -e 'while (true) {}' 2>&1)"
case "$out" in
  "<evaluate>:1: paused on fuel"*) printf '  ok    a runaway loop pauses at its file and line, naming the fuel\n' ;;
  *) printf '  FAIL  a runaway loop pauses at its file and line, naming the fuel\n        got [%s]\n' "$out"
     failures=$((failures + 1)) ;;
esac
status "a paused run exits 5" 5 "$TANG" --fuel 5000 -e 'while (true) {}'
check "the output so far is written when it pauses" "start" \
  "$("$TANG" --fuel 5000 -e 'print("start"); while (true) {}' 2>/dev/null)"
status "a limit reached inside one operation unwinds and exits 6" 6 "$TANG" --fuel 5000 -e 'x = [0, 0, 0, 0] * 20000000;'
status "--fuel without a number exits 2" 2 "$TANG" --fuel
status "--fuel with a word exits 2" 2 "$TANG" --fuel many -e 1
status "--script and --template together exit 2" 2 "$TANG" -s -t -e 1

# The host API: the built-in libraries, the seed, and the error list.
check "use math" "3.141593" "$("$TANG" -s -e 'use math; print(math.pi);')"
check "use math.pi as pi" "3.141593" "$("$TANG" -s -e 'use math.pi as pi; print(pi);')"
check "an unknown library is null and the program goes on" "start end" \
  "$("$TANG" -s -e 'use nothing; print("start "); print(nothing); print("end");')"
first="$("$TANG" --seed 5 -s -e 'use random; print(random.global.next_int);')"
second="$("$TANG" --seed 5 -s -e 'use random; print(random.global.next_int);')"
check "--seed gives the same number twice" "$first" "$second"
other="$("$TANG" --seed 6 -s -e 'use random; print(random.global.next_int);')"
if [ "$first" != "$other" ]; then
  printf '  ok    another seed gives another number\n'
else
  printf '  FAIL  another seed gives another number\n        got [%s] twice\n' "$first"
  failures=$((failures + 1))
fi
check "random.seeded does not depend on the seed" "5777523539921853504" \
  "$("$TANG" --seed 99 -s -e 'use random; print(random.seeded(123).next_int);')"
check "--seed 0 is a seed" "$("$TANG" --seed 0 -s -e 'use random; print(random.global.next_int);')" \
  "$("$TANG" --seed 0 -s -e 'use random; print(random.global.next_int);')"
status "--seed without a number exits 2" 2 "$TANG" --seed
status "--seed with a word exits 2" 2 "$TANG" --seed many -s -e 1
status "--seed with a negative number exits 2" 2 "$TANG" --seed -3 -s -e 1

out="$("$TANG" --errors -s -e 'print(1/0);' 2>&1 >/dev/null)"
check "--errors writes template:file:line: message" "main:<evaluate>:1: Divide by zero" "$out"
status "--errors does not change the exit status of a run that finished" 0 "$TANG" --errors -s -e 'print(1/0);'
check "without --errors nothing is written to stderr" "" "$("$TANG" -s -e 'print(1/0);' 2>&1 >/dev/null)"
out="$("$TANG" --errors -s -e 's = "abc";
s[0] = "x";
print(s);' 2>&1)"
check "a discarded error is listed, with its line" "abcmain:<evaluate>:2: Not supported" "$out"
out="$("$TANG" --errors -s -e 'print(1/0); print(2 % 0);' 2>&1 >/dev/null)"
check "one entry per swallowed error" "main:<evaluate>:1: Divide by zero
main:<evaluate>:1: Modulo by zero" "$out"
out="$("$TANG" --log-errors --errors -s -e 'x = 1/0; print("a");' 2>&1)"
check "--log-errors enters an error that was only stored" "amain:<evaluate>:1: Divide by zero" "$out"
check "the same program without it lists nothing" "a" "$("$TANG" --errors -s -e 'x = 1/0; print("a");' 2>&1)"

out="$("$TANG" --halt-on-error -s -e 'print("a"); x = 1/0; print("b");' 2>/dev/null)"
check "--halt-on-error stops the output at the first error" "a" "$out"
status "--halt-on-error exits 8, the run's ERR_GUEST" 8 "$TANG" --halt-on-error -s -e 'print("a"); x = 1/0; print("b");'
status "--halt-on-error on a clean program exits 0" 0 "$TANG" --halt-on-error -s -e 'print("a");'
out="$("$TANG" --halt-on-error --errors -s -e 'print(1/0);' 2>&1 >/dev/null)"
case "$out" in
  *"main:<evaluate>:1: Divide by zero"*) printf '  ok    --errors lists the error that halted the run\n' ;;
  *) printf '  FAIL  --errors lists the error that halted the run\n        got [%s]\n' "$out"
     failures=$((failures + 1)) ;;
esac
out="$("$TANG" --help)"
case "$out" in
  *"--seed"*"--log-errors"*"--halt-on-error"*"--errors"*) printf '  ok    --help describes --seed, --log-errors, --halt-on-error and --errors\n' ;;
  *) printf '  FAIL  --help describes the host API options\n        got [%s]\n' "$out"
     failures=$((failures + 1)) ;;
esac

# A compile error that is not a syntax error is refused the same way.
out="$("$TANG" -e 'foo(); function foo() {}' 2>&1)"
case "$out" in
  "<evaluate>:1:"*": "*) printf '  ok    a compile error names name:line:column: message\n' ;;
  *) printf '  FAIL  a compile error names name:line:column: message\n        got [%s]\n' "$out"
     failures=$((failures + 1)) ;;
esac
status "a compile error exits 1" 1 "$TANG" -e 'foo(); function foo() {}'
status "global at top level exits 1" 1 "$TANG" -e 'global x;'
status "a slice is not an assignment target" 1 "$TANG" -e 'a = [1]; a[0:1] = 2;'
check "nothing runs after a refusal" "" "$("$TANG" -e 'print("x"); foo(); function foo() {}' 2>/dev/null)"

# A byte that is not valid UTF-8, in the middle of the input but inside a
# comment, which the scanner skips. Read one character at a time into a char,
# as ctang's reader once did, 0xFF is -1 on a platform where char is signed and
# so compares equal to EOF: everything after it was silently dropped.
check "stdin, 0xFF byte inside a comment" "before-after" \
  "$(printf 'print("before");\n/* \377 */\nprint("-after");\n' | "$TANG" -s)"
nodes="$(printf 'print("before");\n/* \377 */\nprint("-after");\n' | "$TANG" --tree -s | grep -c 'Print')"
check "stdin, 0xFF byte: both prints survive" "2" "$nodes"

# Input larger than any single buffer the reader starts with.
big="$(awk 'BEGIN { while (i++ < 5000) printf "print(\"x\");" }' | "$TANG" --tree -s | grep -c 'Print')"
check "stdin, large input" "5000" "$big"
big="$(awk 'BEGIN { while (i++ < 5000) printf "print(\"x\");" }' | "$TANG" -s | wc -c)"
check "stdin, large input runs" "5000" "$big"

# A tree taller than the budget is refused with no tree, run or dumped.
tall="$(awk 'BEGIN { printf "1"; while (i++ < 10000) printf "+1" }')"
exact="$(awk 'BEGIN { printf "1"; while (i++ < 9999) printf "+1" }')"
status "a tree 10,001 deep is refused" 1 "$TANG" -e "$tall"
status "a tree 10,001 deep is refused when only dumped" 1 "$TANG" --tree -e "$tall"
status "a tree 10,000 deep runs" 0 "$TANG" -e "$exact"
check "a deep tree gives its answer (the print and the block add two levels)" "9998" \
  "$("$TANG" -e "print($(awk 'BEGIN { printf "1"; while (i++ < 9997) printf "+1" }'));")"

# Nesting past the parser's own stack is a refusal (exit 1), not a crash and
# not a success.
deep="$(awk 'BEGIN { while (i++ < 20000) printf "(" }')"
status "nesting limit exits 1" 1 sh -c "printf '%s' '$deep' | '$TANG' -s >/dev/null 2>&1"

# A NUL inside a file would end the source early; it is refused.
nulfile="$(mktemp)"
printf 'print(1);\000print(2);' > "$nulfile"
status "a NUL byte in a file exits 3" 3 "$TANG" -s "$nulfile"
rm -f "$nulfile"

# A syntax error: name:line:column: message on stderr, nothing on stdout, and
# exit 1. The position is 1-based.
out="$("$TANG" -s "$CORPUS/script/reject-syntax-error.tang" 2>&1 >/dev/null)"
case "$out" in
  "$CORPUS/script/reject-syntax-error.tang:1:5: syntax error"*) printf '  ok    syntax error names file:line:column: message\n' ;;
  *) printf '  FAIL  syntax error names file:line:column: message\n        got [%s]\n' "$out"
     failures=$((failures + 1)) ;;
esac
check "syntax error prints no tree" "" "$("$TANG" -s "$CORPUS/script/reject-syntax-error.tang" 2>/dev/null)"
status "syntax error exits 1" 1 "$TANG" -s "$CORPUS/script/reject-syntax-error.tang"

out="$(printf 'a = 1;\nb = ;\n' | "$TANG" -s 2>&1 >/dev/null)"
check "stdin error names <stdin> and the line" "<stdin>:2:5: syntax error, unexpected ;" "$out"
out="$("$TANG" -s -e 'x = ;' 2>&1 >/dev/null)"
check "-e error names <evaluate>" "<evaluate>:1:5: syntax error, unexpected ;" "$out"

# The refusals of section 10.1 all exit 1.
for f in unterminated-string trailing-backslash octal-over-255 template-end-in-script stray-character invalid-utf8-string integer-too-large compound-assign-on-index truncated-block; do
  status "script refusal $f exits 1" 1 "$TANG" -s "$CORPUS/script/reject-$f.tang"
done
status "template refusal exits 1" 1 "$TANG" "$CORPUS/template/reject-comment-tag.tang"

# A file that cannot be read is reported, with its own status, rather than
# read as empty. A directory opens successfully and fails on the first read,
# so it is the case that a check on fopen() alone lets through.
out="$("$TANG" -s /nonexistent/no-such-file.tang 2>&1)"
case "$out" in
  *"failed to read the file"*) printf '  ok    missing file is reported\n' ;;
  *) printf '  FAIL  missing file is reported\n        got [%s]\n' "$out"
     failures=$((failures + 1)) ;;
esac
status "missing file exits 3" 3 "$TANG" -s /nonexistent/no-such-file.tang
out="$("$TANG" -s "$HERE" 2>&1)"
case "$out" in
  *"failed to read the file"*) printf '  ok    directory is reported\n' ;;
  *) printf '  FAIL  directory is reported\n        got [%s]\n' "$out"
     failures=$((failures + 1)) ;;
esac
status "directory exits 3" 3 "$TANG" -s "$HERE"

# Usage errors exit 2.
status "-e without an argument exits 2" 2 "$TANG" -e
status "two files exit 2" 2 "$TANG" a b
status "a file and -e exit 2" 2 "$TANG" -e 1 "$CORPUS/script/empty.tang"
status "an unknown option exits 2" 2 "$TANG" --no-such-option

# Help.
out="$("$TANG" --help)"
case "$out" in
  *"Run a Tang template"*"--tree"*"--fuel"*"--depth"*) printf '  ok    --help describes running, --tree, --fuel and --depth\n' ;;
  *) printf '  FAIL  --help describes running, --tree, --fuel and --depth\n        got [%s]\n' "$out"
     failures=$((failures + 1)) ;;
esac
status "-h exits 0" 0 "$TANG" -h
status "--cleanup is accepted" 0 "$TANG" -c -s -e '1'

if [ "$failures" -ne 0 ]; then
  printf '\n%s CLI check(s) failed.\n' "$failures"
  exit 1
fi
printf '\nAll CLI checks passed.\n'
