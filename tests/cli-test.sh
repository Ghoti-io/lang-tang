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

# A script and a template read from a file.
check "file, script" "$TREE" "$("$TANG" -s "$CORPUS/script/expression-only.tang" | head -5)"
check "file, template: text becomes a print" \
  "Block:" "$("$TANG" "$CORPUS/template/plain-text.tang" | head -1)"

# The source from the command line.
check "-e, script" "$TREE" "$("$TANG" -s -e '1 + 2')"
check "--evaluate --script" "$TREE" "$("$TANG" --script --evaluate '1 + 2')"
check "-e, template" "Block:" "$("$TANG" -e 'Hello <%= 1+1 %>!' | head -1)"

# The same, read from stdin.
check "stdin, script" "$TREE" "$(printf '1 + 2' | "$TANG" -s)"
check "stdin, template" "Block:" "$(printf 'Hello <%%= 1+1 %%>!' | "$TANG" | head -1)"

# Empty stdin is an empty tree, not a read of whatever followed the buffer in
# memory: no output and a success.
check "stdin, empty" "" "$(printf '' | "$TANG" -s)"
status "stdin, empty exits 0" 0 sh -c "printf '' | '$TANG'"
check "-e '', empty" "" "$("$TANG" -s -e '')"

# A byte that is not valid UTF-8, in the middle of the input but inside a
# comment, which the scanner skips. Read one character at a time into a char,
# as ctang's reader once did, 0xFF is -1 on a platform where char is signed and
# so compares equal to EOF: everything after it was silently dropped.
check "stdin, 0xFF byte inside a comment" "Block:" \
  "$(printf 'print("before");\n/* \377 */\nprint("-after");\n' | "$TANG" -s | head -1)"
nodes="$(printf 'print("before");\n/* \377 */\nprint("-after");\n' | "$TANG" -s | grep -c 'Print')"
check "stdin, 0xFF byte: both prints survive" "2" "$nodes"

# Input larger than any single buffer the reader starts with.
big="$(awk 'BEGIN { while (i++ < 5000) printf "print(\"x\");" }' | "$TANG" -s | grep -c 'Print')"
check "stdin, large input" "5000" "$big"

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
  *"Execution arrives with"*"interpreter"*) printf '  ok    --help says execution arrives with the interpreter\n' ;;
  *) printf '  FAIL  --help says execution arrives with the interpreter\n        got [%s]\n' "$out"
     failures=$((failures + 1)) ;;
esac
status "-h exits 0" 0 "$TANG" -h
status "--cleanup is accepted" 0 "$TANG" -c -s -e '1'

if [ "$failures" -ne 0 ]; then
  printf '\n%s CLI check(s) failed.\n' "$failures"
  exit 1
fi
printf '\nAll CLI checks passed.\n'
