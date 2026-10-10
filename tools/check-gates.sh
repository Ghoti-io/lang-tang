#!/bin/sh
#
# Prove that each layering gate fails on the defect it exists to catch.
#
# A gate that has never been seen to fail may be measuring nothing. So this
# runs the real scripts - not copies, not mocks - against tests/gates:
#
#   control/   a tree that is correct; the gate must pass it, so that a gate
#              which rejects everything cannot pass this self-test
#   planted-*  a tree with one defect; the gate must exit non-zero AND name
#              the offending file, line or edge, so that a gate which fails
#              for the wrong reason (a typo, a missing tool) does not count
#   (empty)    an empty directory; the gate must fail rather than report
#              success over a population of zero
#
# The link-line check cannot use a committed fixture, because what it reads is
# a built object's NEEDED list. So it builds real ones in a temporary
# directory, against stub libraries: a shared object and a program that link
# ctang, one that links the debugger, one that links the JIT, and controls that
# link only what is allowed.
#
# Usage: make check-gates   (or tools/check-gates.sh)

set -u

# The gates are shown in both of the JIT arms below, by setting the variable
# per check; the ambient value must not decide a check that does not set it.
unset GLTANG_EDGES_JIT

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"
FIX="$ROOT/tests/gates"
CC="${CC:-cc}"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT INT TERM

failures=0
checks=0

fail() {
  printf 'check-gates: FAIL: %s\n' "$*" >&2
  failures=$((failures + 1))
}

# expect_pass <label> <command...>
expect_pass() {
  label="$1"; shift
  checks=$((checks + 1))
  if out="$("$@" 2>&1)"; then
    printf '  ok   %s passes\n' "$label"
  else
    fail "$label should pass but failed:
$out"
  fi
}

# expect_fail <label> <needle> <command...>
# The needle is what the failure message must name.
expect_fail() {
  label="$1"; needle="$2"; shift 2
  checks=$((checks + 1))
  out="$("$@" 2>&1)"
  rc=$?
  if [ "$rc" -eq 0 ]; then
    fail "$label should fail but exited 0:
$out"
  elif ! printf '%s' "$out" | grep -qF -- "$needle"; then
    fail "$label failed, but without naming '$needle':
$out"
  else
    printf '  ok   %s fails, naming %s\n' "$label" "$needle"
  fi
}

mkdir "$work/empty"

printf 'check-labels\n'
L="$HERE/check-labels.sh"
expect_pass 'labels/control' "$L" "$FIX/labels/control"
expect_fail 'labels/planted-missing (no label)' 'parse.h has no @stability' \
  "$L" "$FIX/labels/planted-missing"
expect_fail 'labels/planted-bogus' '"experimental"' "$L" "$FIX/labels/planted-bogus"
expect_fail 'labels/planted-prose-only' 'core.h has no @stability' \
  "$L" "$FIX/labels/planted-prose-only"
expect_fail 'labels/planted-two-labels' 'more than one @stability' \
  "$L" "$FIX/labels/planted-two-labels"
expect_fail 'labels/planted-free-in-stable (free where stable is required)' \
  'core.h is labelled free but must be stable' "$L" "$FIX/labels/planted-free-in-stable"
expect_fail 'labels/planted-stable-in-free (stable where free is required)' \
  'astNode.h is labelled stable but must be free' "$L" "$FIX/labels/planted-stable-in-free"
expect_fail 'labels/planted-stable-engine (the execution API labelled stable)' \
  'execution.h is labelled stable but must be free' "$L" "$FIX/labels/planted-stable-engine"
expect_fail 'labels/planted-stable-library (the host library API labelled stable)' \
  'library.h is labelled stable but must be free' "$L" "$FIX/labels/planted-stable-library"
expect_fail 'labels/planted-free-seeds (the seed sequence labelled free)' \
  'seeds.h is labelled free but must be stable' "$L" "$FIX/labels/planted-free-seeds"
expect_fail 'labels/planted-unclassified' 'newthing.h is in neither' \
  "$L" "$FIX/labels/planted-unclassified"
expect_fail 'labels/empty' 'measuring nothing' "$L" "$work/empty"

printf 'check-vscode\n'
V="$HERE/check-vscode.sh"
expect_pass 'vscode/control' "$V" "$FIX/vscode/control"
expect_pass 'vscode/the contribution as it is' "$V" "$ROOT/editors/vscode"
vf() { expect_fail "vscode/$1" "$2" "$V" "$FIX/vscode/$1"; }
vf planted-bad-json 'is not valid JSON'
vf planted-no-debugger 'no debugger of type "tang"'
vf planted-no-program 'do not require a "program"'
vf planted-no-script-attribute 'no boolean "script"'
vf planted-script-defaults-true 'no boolean "script"'
vf planted-no-initial-configurations 'no initialConfigurations'
vf planted-no-breakpoints 'contributes.breakpoints'
vf planted-no-main 'missing.js, which does not exist'
vf planted-no-main-key 'has no "main"'
vf planted-wrong-command '"tang-old", not "tang"'
vf planted-no-dap 'do not include "--dap"'
vf planted-no-file 'the identifier `file`'
vf planted-script-only 'expected one adapter call with "--script"'
vf planted-script-only 'a template, which the checkpoint steps), found 0'
vf planted-no-register 'does not register a descriptor factory'
expect_fail 'vscode/empty' 'checks nothing' "$V" "$work/empty"

printf 'check-stamps\n'
# The real Makefile with `JIT=$(JIT)` taken out of its stamps: the gate must
# name JIT. The unmodified Makefile is the control.
expect_pass 'stamps/control (the Makefile as it is)' python3 "$HERE/check-stamps.py" "$ROOT/Makefile"
sed 's/ JIT=\$(JIT)//g' "$ROOT/Makefile" > "$work/Makefile.nojit-stamp"
expect_fail 'stamps/planted (no stamp records JIT)' 'does not record $(JIT)' python3 "$HERE/check-stamps.py" "$work/Makefile.nojit-stamp"

printf 'make options\n'
# The JIT option (story 15): an invalid value, and a missing runtime-jit, are
# hard errors that name themselves, and JIT=no needs no runtime-jit. The other
# dependencies are stubs, so that the one under test is the first one missing.
pcdir="$work/pc"
mkdir -p "$pcdir"
for m in cutil unicode runtime-core runtime-heap; do
  printf 'Name: %s\nDescription: stub\nVersion: 0\nCflags: -I/nonexistent\nLibs: -lnone\n' "$m" > "$pcdir/ghoti.io-$m-0.pc"
done
mk() { env -u MAKEFLAGS -u MFLAGS -u MAKELEVEL -u PREFIX -u JIT PKG_CONFIG_PATH="$pcdir" make -C "$ROOT" -n all WITH_DEBUG=no PC_INSTALL_PATH="$pcdir" "$@"; }
expect_fail 'make/JIT=maybe is refused' 'JIT must be yes or no' mk JIT=maybe
expect_fail 'make/the default build without runtime-jit is refused, naming the fix' 'runtime-jit was not found' mk
expect_pass 'make/JIT=no needs no runtime-jit' mk JIT=no

printf 'check-edges --includes\n'
E="$HERE/check-edges.sh"
expect_pass 'edges/control (clean, with a ctang include under tests/)' "$E" --includes "$FIX/edges/control"
expect_fail 'edges/planted-ctang' 'lang-tang -> tang' "$E" --includes "$FIX/edges/planted-ctang"
expect_fail 'edges/planted-ctang names the file' 'planted-ctang/src/x.c' \
  "$E" --includes "$FIX/edges/planted-ctang"
expect_fail 'edges/planted-ctang-quoted' 'lang-tang -> tang' \
  "$E" --includes "$FIX/edges/planted-ctang-quoted"
expect_fail 'edges/planted-ctang-spaced' 'lang-tang -> tang' \
  "$E" --includes "$FIX/edges/planted-ctang-spaced"
expect_fail 'edges/planted-ctang-header' 'y.h' "$E" --includes "$FIX/edges/planted-ctang-header"
expect_fail 'edges/planted-ctang-bison' 'p.y' "$E" --includes "$FIX/edges/planted-ctang-bison"
expect_fail 'edges/planted-ctang-flex' 's.l' "$E" --includes "$FIX/edges/planted-ctang-flex"
expect_fail 'edges/planted-ctang-bench' 'b.c' "$E" --includes "$FIX/edges/planted-ctang-bench"
expect_fail 'edges/planted-ctang-example' 'e.c' "$E" --includes "$FIX/edges/planted-ctang-example"
expect_fail 'edges/planted-debug' 'lang-tang -> runtime-debug' "$E" --includes "$FIX/edges/planted-debug"
expect_fail 'edges/planted-debug names the file' 'planted-debug/src/x.c' "$E" --includes "$FIX/edges/planted-debug"
# The hosts (story 13): src/tang.c and examples/web_server.c may include the
# debugger and text; nothing else may, whatever it is called or where it is.
expect_pass 'edges/control-hosts (the two hosts include runtime-debug and text)' "$E" --includes "$FIX/edges/control-hosts"
expect_fail 'edges/planted-debug-example (a non-host example includes runtime-debug)' 'planted-debug-example/examples/e.c' \
  "$E" --includes "$FIX/edges/planted-debug-example"
expect_fail 'edges/planted-debug-example names the edge' 'lang-tang -> runtime-debug' \
  "$E" --includes "$FIX/edges/planted-debug-example"
expect_fail 'edges/planted-text (a library source includes text)' 'lang-tang -> text' "$E" --includes "$FIX/edges/planted-text"
expect_fail 'edges/planted-host-name (a host name in the wrong directory)' 'planted-host-name/examples/tang.c' \
  "$E" --includes "$FIX/edges/planted-host-name"
expect_fail 'edges/planted-debug-header (a public header includes the debugger)' 'z.h' \
  "$E" --includes "$FIX/edges/planted-debug-header"
expect_fail 'edges/planted-jit' 'lang-tang -> runtime-jit' "$E" --includes "$FIX/edges/planted-jit"
# The JIT arm (story 15): with JIT=yes only src/jit/ may include runtime-jit;
# with JIT=no nothing may, which is how the interpreter-only arm is shown not to
# reach the code generator. The same tree is a control in one arm and a defect
# in the other.
expect_pass 'edges/control-jit with JIT=yes (src/jit/ includes runtime-jit)' env GLTANG_EDGES_JIT=yes "$E" --includes "$FIX/edges/control-jit"
expect_pass 'edges/control-jit with JIT=no (src/jit/ is not in the arm, so its include is not read)' env GLTANG_EDGES_JIT=no "$E" --includes "$FIX/edges/control-jit"
expect_fail 'edges/planted-jit-elsewhere with JIT=no (outside src/jit/ nothing may name runtime-jit)' 'planted-jit-elsewhere/src/vm/x.c' \
  env GLTANG_EDGES_JIT=no "$E" --includes "$FIX/edges/planted-jit-elsewhere"
expect_fail 'edges/planted-jit-elsewhere with JIT=yes (a runtime-jit include outside src/jit/)' 'planted-jit-elsewhere/src/vm/x.c' \
  env GLTANG_EDGES_JIT=yes "$E" --includes "$FIX/edges/planted-jit-elsewhere"
expect_fail 'edges/planted-jit with JIT=yes (an include outside src/jit/)' 'planted-jit/src/x.c' \
  env GLTANG_EDGES_JIT=yes "$E" --includes "$FIX/edges/planted-jit"
expect_fail 'edges/an invalid mode' 'must be yes or no' env GLTANG_EDGES_JIT=maybe "$E" --includes "$FIX/edges/control"
expect_fail 'edges/planted-engine' 'lang-tang -> lang-wasm' "$E" --includes "$FIX/edges/planted-engine"
expect_fail 'edges/planted-binary-h' 'binary.h' "$E" --includes "$FIX/edges/planted-binary-h"
expect_fail 'edges/planted-binary-h-quoted' 'binary.h' "$E" --includes "$FIX/edges/planted-binary-h-quoted"
expect_fail 'edges/planted-binary-h-ctang' 'binary.h' "$E" --includes "$FIX/edges/planted-binary-h-ctang"
expect_fail 'edges/planted-no-include-dir (a population that is missing half)' 'measuring nothing' \
  "$E" --includes "$FIX/edges/planted-no-include-dir"
expect_fail 'edges/empty (includes)' 'measuring nothing' "$E" --includes "$work/empty"

expect_fail 'edges/planted-relative' 'lang-tang -> tang' \
  "$E" --includes "$FIX/edges/planted-relative"

printf 'check-edges --links\n'
# The .dll arm (objdump -p is the reader there) has run under wine, cross-built
# (suite/tools/xwin in the workspace); it has not run on a Windows machine.
# EXE is the suffix the compiler puts on a program whatever -o was told: without
# it the fixtures were built and then not found under the names they were given.
case "$(uname -s)" in
  MINGW* | MSYS*) SHEXT=dll; SHFLAGS="-shared"; EXE=.exe ;;
  Darwin) SHEXT=dylib; SHFLAGS="-dynamiclib"; EXE= ;;
  *) SHEXT=so; SHFLAGS="-shared -fPIC"; EXE= ;;
esac
stubs="$work/stubs"
mkdir -p "$stubs" "$work/planted-tang" "$work/planted-debug" "$work/planted-jit" "$work/control" "$work/dev" "$work/exe-planted" "$work/exe-control" "$work/host-ok" "$work/host-bad" "$work/other-bad" "$work/so-text"
printf 'int stub_tang(void) { return 1; }\n' > "$work/tang.c"
printf 'int stub_debug(void) { return 2; }\n' > "$work/debug.c"
printf 'int stub_jit(void) { return 3; }\n' > "$work/jit.c"
printf 'int stub_cutil(void) { return 4; }\n' > "$work/cutil.c"
printf 'int stub_unicode(void) { return 5; }\n' > "$work/unicode.c"
printf 'int stub_self(void) { return 6; }\n' > "$work/self.c"
printf 'int stub_core(void) { return 7; }\n' > "$work/core.c"
printf 'int stub_heap(void) { return 8; }\n' > "$work/heap.c"
printf 'int stub_tang(void);\nint planted_tang(void) { return stub_tang(); }\n' > "$work/planted_tang.c"
printf 'int stub_debug(void);\nint planted_debug(void) { return stub_debug(); }\n' > "$work/planted_debug.c"
printf 'int stub_jit(void);\nint planted_jit(void) { return stub_jit(); }\n' > "$work/planted_jit.c"
printf 'int stub_cutil(void);\nint stub_unicode(void);\nint stub_core(void);\nint stub_heap(void);\nint control(void) { return stub_cutil() + stub_unicode() + stub_core() + stub_heap(); }\n' > "$work/control.c"
printf 'int stub_cutil(void);\nint dev(void) { return stub_cutil(); }\n' > "$work/dev.c"
printf 'int stub_tang(void);\nint stub_self(void);\nint main(void) { return stub_tang() + stub_self(); }\n' > "$work/exe_planted.c"
printf 'int stub_cutil(void);\nint stub_unicode(void);\nint stub_core(void);\nint stub_heap(void);\nint stub_self(void);\nint main(void) { return stub_cutil() + stub_unicode() + stub_core() + stub_heap() + stub_self(); }\n' > "$work/exe_control.c"

printf 'int stub_text(void) { return 9; }\n' > "$work/text.c"
printf 'int stub_chron(void) { return 10; }\n' > "$work/chron.c"
printf 'int stub_regex(void) { return 11; }\n' > "$work/regex.c"
printf 'int stub_debug(void);\nint stub_text(void);\nint stub_chron(void);\nint stub_regex(void);\nint stub_cutil(void);\nint stub_self(void);\nint main(void) { return stub_debug() + stub_text() + stub_chron() + stub_regex() + stub_cutil() + stub_self(); }\n' > "$work/host_ok.c"
printf 'int stub_jit(void);\nint stub_self(void);\nint main(void) { return stub_jit() + stub_self(); }\n' > "$work/host_bad.c"
printf 'int stub_debug(void);\nint stub_self(void);\nint main(void) { return stub_debug() + stub_self(); }\n' > "$work/other_bad.c"
printf 'int stub_text(void);\nint planted_text(void) { return stub_text(); }\n' > "$work/planted_text.c"

built=1
# shellcheck disable=SC2086
{
  $CC $SHFLAGS -o "$stubs/libghoti.io-tang-0.$SHEXT" "$work/tang.c" &&
  $CC $SHFLAGS -o "$stubs/libghoti.io-runtime-debug-0.$SHEXT" "$work/debug.c" &&
  $CC $SHFLAGS -o "$stubs/libghoti.io-runtime-jit-0.$SHEXT" "$work/jit.c" &&
  $CC $SHFLAGS -o "$stubs/libghoti.io-cutil-0.$SHEXT" "$work/cutil.c" &&
  $CC $SHFLAGS -o "$stubs/libghoti.io-unicode-0.$SHEXT" "$work/unicode.c" &&
  $CC $SHFLAGS -o "$stubs/libghoti.io-lang-tang-0.$SHEXT" "$work/self.c" &&
  $CC $SHFLAGS -o "$stubs/libghoti.io-runtime-core-0.$SHEXT" "$work/core.c" &&
  $CC $SHFLAGS -o "$stubs/libghoti.io-runtime-heap-0.$SHEXT" "$work/heap.c" &&
  $CC $SHFLAGS -o "$stubs/libghoti.io-cutil-dev.$SHEXT" "$work/cutil.c" &&
  $CC $SHFLAGS -o "$work/planted-tang/libplanted.$SHEXT" "$work/planted_tang.c" \
    -L"$stubs" -Wl,--no-as-needed -l:libghoti.io-tang-0.$SHEXT &&
  $CC $SHFLAGS -o "$work/planted-debug/libplanted.$SHEXT" "$work/planted_debug.c" \
    -L"$stubs" -Wl,--no-as-needed -l:libghoti.io-runtime-debug-0.$SHEXT &&
  $CC $SHFLAGS -o "$work/planted-jit/libplanted.$SHEXT" "$work/planted_jit.c" \
    -L"$stubs" -Wl,--no-as-needed -l:libghoti.io-runtime-jit-0.$SHEXT &&
  $CC $SHFLAGS -o "$work/control/libcontrol.$SHEXT" "$work/control.c" \
    -L"$stubs" -Wl,--no-as-needed -l:libghoti.io-cutil-0.$SHEXT \
    -l:libghoti.io-unicode-0.$SHEXT -l:libghoti.io-runtime-core-0.$SHEXT \
    -l:libghoti.io-runtime-heap-0.$SHEXT &&
  $CC $SHFLAGS -o "$work/dev/libdev.$SHEXT" "$work/dev.c" \
    -L"$stubs" -Wl,--no-as-needed -l:libghoti.io-cutil-dev.$SHEXT &&
  $CC -o "$work/exe-planted/tang$EXE" "$work/exe_planted.c" \
    -L"$stubs" -Wl,--no-as-needed -l:libghoti.io-tang-0.$SHEXT \
    -l:libghoti.io-lang-tang-0.$SHEXT &&
  $CC -o "$work/exe-control/tang$EXE" "$work/exe_control.c" \
    -L"$stubs" -Wl,--no-as-needed -l:libghoti.io-cutil-0.$SHEXT \
    -l:libghoti.io-unicode-0.$SHEXT -l:libghoti.io-runtime-core-0.$SHEXT \
    -l:libghoti.io-runtime-heap-0.$SHEXT -l:libghoti.io-lang-tang-0.$SHEXT &&
  $CC $SHFLAGS -o "$stubs/libghoti.io-text-0.$SHEXT" "$work/text.c" &&
  $CC $SHFLAGS -o "$stubs/libghoti.io-chron-0.$SHEXT" "$work/chron.c" &&
  $CC $SHFLAGS -o "$stubs/libghoti.io-regex-0.$SHEXT" "$work/regex.c" &&
  $CC -o "$work/host-ok/tang$EXE" "$work/host_ok.c" \
    -L"$stubs" -Wl,--no-as-needed -l:libghoti.io-runtime-debug-0.$SHEXT -l:libghoti.io-text-0.$SHEXT \
    -l:libghoti.io-chron-0.$SHEXT -l:libghoti.io-regex-0.$SHEXT -l:libghoti.io-cutil-0.$SHEXT \
    -l:libghoti.io-lang-tang-0.$SHEXT &&
  cp "$work/host-ok/tang$EXE" "$work/host-ok/web_server$EXE" &&
  $CC -o "$work/host-bad/tang$EXE" "$work/host_bad.c" \
    -L"$stubs" -Wl,--no-as-needed -l:libghoti.io-runtime-jit-0.$SHEXT -l:libghoti.io-lang-tang-0.$SHEXT &&
  $CC -o "$work/other-bad/pause_resume$EXE" "$work/other_bad.c" \
    -L"$stubs" -Wl,--no-as-needed -l:libghoti.io-runtime-debug-0.$SHEXT -l:libghoti.io-lang-tang-0.$SHEXT &&
  $CC $SHFLAGS -o "$work/so-text/libplanted.$SHEXT" "$work/planted_text.c" \
    -L"$stubs" -Wl,--no-as-needed -l:libghoti.io-text-0.$SHEXT
} >"$work/build.log" 2>&1 || built=0
if [ "$built" -eq 0 ]; then
  fail "could not build the link-line fixtures:
$(cat "$work/build.log")"
else
  expect_pass 'links/control (cutil, unicode, runtime-core, runtime-heap)' "$E" --links "$work/control"
  expect_pass 'links/cutil with a BRANCH suffix (-dev)' "$E" --links "$work/dev"
  expect_pass 'links/program control (cutil, unicode, runtime-core, runtime-heap, lang-tang)' \
    "$E" --links "$work/exe-control/tang$EXE"
  expect_fail 'links/planted-tang (a shared object linking ctang)' 'lang-tang -> tang' \
    "$E" --links "$work/planted-tang"
  expect_fail 'links/planted-tang names the object' 'libplanted' \
    "$E" --links "$work/planted-tang"
  expect_fail 'links/planted-debug' 'lang-tang -> runtime-debug' "$E" --links "$work/planted-debug"
  expect_fail 'links/planted-jit' 'lang-tang -> runtime-jit' "$E" --links "$work/planted-jit"
  expect_pass 'links/planted-jit with JIT=yes (the shared object may link runtime-jit)' env GLTANG_EDGES_JIT=yes "$E" --links "$work/planted-jit"
  expect_fail 'links/planted-tang with JIT=yes (only runtime-jit is added)' 'lang-tang -> tang' env GLTANG_EDGES_JIT=yes "$E" --links "$work/planted-tang"
  expect_pass 'links/host tang (runtime-debug, text and what text needs are allowed in the tang command)' \
    "$E" --links "$work/host-ok/tang$EXE"
  expect_pass 'links/host web_server (the same, in the web-server example)' "$E" --links "$work/host-ok/web_server$EXE"
  expect_fail 'links/a host may not link anything else (the tang command linking the JIT)' 'lang-tang -> runtime-jit' \
    "$E" --links "$work/host-bad/tang$EXE"
  expect_fail 'links/a program that is not a host may not link runtime-debug (another example)' \
    'lang-tang -> runtime-debug' "$E" --links "$work/other-bad/pause_resume$EXE"
  expect_fail 'links/the non-host program is named' 'other-bad/pause_resume' \
    "$E" --links "$work/other-bad/pause_resume$EXE"
  expect_fail 'links/a shared object linking text (the library is never a host)' 'lang-tang -> text' \
    "$E" --links "$work/so-text"
  expect_fail 'links/planted-tang-program (the tang command linking ctang)' 'lang-tang -> tang' \
    "$E" --links "$work/exe-planted/tang$EXE"
  expect_fail 'links/planted-tang-program names the program' 'exe-planted/tang' \
    "$E" --links "$work/exe-planted/tang$EXE"
fi
expect_fail 'links/empty' 'measuring nothing' "$E" --links "$work/empty"

# A name that merely begins with an allowed one is another library, not that
# library with a branch suffix.
extra="$work/extra"
mkdir -p "$extra" "$work/extra-stubs"
printf 'int stub_extra(void) { return 4; }\n' > "$work/extra-stub.c"
printf 'int stub_extra(void);\nint extended(void) { return stub_extra(); }\n' > "$work/extended.c"
# shellcheck disable=SC2086
if $CC $SHFLAGS -o "$work/extra-stubs/libghoti.io-cutil-extra-0.$SHEXT" "$work/extra-stub.c" &&
  $CC $SHFLAGS -o "$extra/libextended.$SHEXT" "$work/extended.c" \
    -L"$work/extra-stubs" -Wl,--no-as-needed -l:libghoti.io-cutil-extra-0.$SHEXT \
    >"$work/extra-build.log" 2>&1; then
  expect_fail 'links/planted-name-extending-an-allowed-one' 'lang-tang -> cutil-extra' \
    "$E" --links "$extra"
else
  fail "could not build the extended-name fixture"
fi
expect_fail 'links/a path that does not exist' 'does not exist' "$E" --links "$work/no-such-thing"

printf 'check-fp-contract\n'
# -ffp-contract=off is what makes a compiled float the interpreter's float on
# every host. The real Makefile is the control; the same Makefile with the flag
# taken out, everywhere or from one flag set, is the planted defect and must
# name the library and the set.
F="$HERE/check-fp-contract.sh"
lib="$(basename "$ROOT")"
expect_pass 'fp-contract/control (the Makefile as it is)' "$F" "$ROOT/Makefile"
sed 's/ -ffp-contract=off//g' "$ROOT/Makefile" > "$work/Makefile.nofp"
expect_fail 'fp-contract/planted (the flag removed everywhere)' "$lib: CFLAGS does not name -ffp-contract=off" "$F" "$work/Makefile.nofp" "$ROOT"
expect_fail 'fp-contract/planted (the flag removed everywhere, C++)' "$lib: CXXFLAGS does not name -ffp-contract=off" "$F" "$work/Makefile.nofp" "$ROOT"
sed '/^CXXFLAGS :=/s/ -ffp-contract=off//' "$ROOT/Makefile" > "$work/Makefile.nofp-cxx"
expect_fail 'fp-contract/planted (removed from CXXFLAGS only)' "$lib: CXXFLAGS does not name -ffp-contract=off" "$F" "$work/Makefile.nofp-cxx" "$ROOT"
sed '/^CFLAGS :=/s/ -ffp-contract=off//' "$ROOT/Makefile" > "$work/Makefile.nofp-c"
expect_fail 'fp-contract/planted (removed from CFLAGS only, so LIB_CFLAGS and the sanitizer sets too)' "$lib: LIB_CFLAGS does not name -ffp-contract=off" "$F" "$work/Makefile.nofp-c" "$ROOT"
expect_pass 'fp-contract/fixture control' "$F" "$FIX/fp-contract/control.mk"
expect_fail 'fp-contract/planted-cflags' 'fixture: CFLAGS does not name' "$F" "$FIX/fp-contract/planted-cflags.mk"
expect_fail 'fp-contract/planted-cxxflags' 'fixture: CXXFLAGS does not name' "$F" "$FIX/fp-contract/planted-cxxflags.mk"
expect_fail 'fp-contract/planted-lib-flags' 'fixture: LIB_CFLAGS does not name' "$F" "$FIX/fp-contract/planted-lib-flags.mk"
expect_fail 'fp-contract/planted-sanitizer-set' 'fixture: TSAN_CXXFLAGS does not name' "$F" "$FIX/fp-contract/planted-sanitizer-set.mk"
expect_fail 'fp-contract/planted-overridden (a later =fast wins)' 'uses -ffp-contract=fast' "$F" "$FIX/fp-contract/planted-overridden.mk"
expect_fail 'fp-contract/planted-fast-math' 'uses -ffast-math' "$F" "$FIX/fp-contract/planted-fast-math.mk"
expect_pass 'fp-contract/control with an EXTRA_CFLAGS that is harmless' env EXTRA_CFLAGS=-O3 EXTRA_CXXFLAGS=-O3 "$F" "$ROOT/Makefile"
expect_fail 'fp-contract/planted (EXTRA_CFLAGS=-ffp-contract=fast on the real build)' "$lib: CFLAGS uses -ffp-contract=fast" env EXTRA_CFLAGS=-ffp-contract=fast "$F" "$ROOT/Makefile"
expect_fail 'fp-contract/planted (EXTRA_CXXFLAGS=-ffast-math on the real build)' "$lib: CXXFLAGS uses -ffast-math" env EXTRA_CXXFLAGS=-ffast-math "$F" "$ROOT/Makefile"
expect_pass 'fp-contract/fixture with an EXTRA_CFLAGS that is harmless' env EXTRA_CFLAGS=-O3 "$F" "$FIX/fp-contract/extra.mk"
expect_fail 'fp-contract/planted (EXTRA_CFLAGS overrides the contraction mode)' 'fixture: CFLAGS uses -ffp-contract=on' env EXTRA_CFLAGS=-ffp-contract=on "$F" "$FIX/fp-contract/extra.mk"
expect_fail 'fp-contract/planted (EXTRA_CXXFLAGS=-freciprocal-math)' 'fixture: CXXFLAGS uses -freciprocal-math' env EXTRA_CXXFLAGS=-freciprocal-math "$F" "$FIX/fp-contract/extra.mk"
expect_fail 'fp-contract/planted-unsafe-math' 'uses -funsafe-math-optimizations' "$F" "$FIX/fp-contract/planted-unsafe-math.mk"
expect_fail 'fp-contract/planted-no-flag-sets' 'measuring nothing' "$F" "$FIX/fp-contract/planted-no-flag-sets.mk"
expect_fail 'fp-contract/empty' 'measuring nothing' "$F" "$work/empty/Makefile"

printf 'check-stamps (fixtures)\n'
S="$HERE/check-stamps.py"
expect_pass 'stamps/control' python3 "$S" "$FIX/stamps/control.mk"
expect_fail 'stamps/planted-no-stamp' 'names no flag stamp' \
  python3 "$S" "$FIX/stamps/planted-no-stamp.mk"
expect_fail 'stamps/planted-unrecorded-flag' 'EXTRA_CFLAGS' \
  python3 "$S" "$FIX/stamps/planted-unrecorded-flag.mk"
expect_fail 'stamps/planted-link-unrecorded' 'LINK_EXTRA' \
  python3 "$S" "$FIX/stamps/planted-link-unrecorded.mk"
expect_fail 'stamps/planted-no-printf' 'does not printf' \
  python3 "$S" "$FIX/stamps/planted-no-printf.mk"
expect_fail 'stamps/planted-no-stamps' 'no flag stamps at all' \
  python3 "$S" "$FIX/stamps/planted-no-stamps.mk"
expect_fail 'stamps/planted-no-compile' 'no compile rules' \
  python3 "$S" "$FIX/stamps/planted-no-compile.mk"
expect_fail 'stamps/planted-no-link' 'no link lines' \
  python3 "$S" "$FIX/stamps/planted-no-link.mk"

# check-symbols reads a built shared object's dynamic symbol table, which is
# nm -D and so Linux only (the Makefile skips it elsewhere too).
if [ "$(uname -s)" = Linux ]; then
  printf 'check-symbols\n'
  Y="$HERE/check-symbols.sh"
  tok=ghotiio_lang_tang_0
  sym="$work/sym"
  mkdir -p "$sym"
  printf 'int %s_gltang_ctx_make(void) { return 1; }\n' "$tok" > "$sym/good.c"
  printf 'int gltang_leaked(void) { return 1; }\n' > "$sym/bad.c"
  printf 'int %s_gltang_ctx_make(void) { return 1; }\nint %s_gltang_missing(void);\nint %s_gltang_user(void) { return %s_gltang_missing(); }\n' \
    "$tok" "$tok" "$tok" "$tok" > "$sym/split.c"
  printf 'int %s_gltang_other(void) { return 1; }\n' "$tok" > "$sym/other.c"
  printf 'static int hidden(void) { return 1; }\n' > "$sym/empty.c"
  built=1
  {
    $CC -shared -fPIC -o "$sym/good.so" "$sym/good.c" &&
    $CC -shared -fPIC -o "$sym/bad.so" "$sym/bad.c" &&
    $CC -shared -fPIC -o "$sym/split.so" "$sym/split.c" &&
    $CC -shared -fPIC -o "$sym/other.so" "$sym/other.c" &&
    $CC -shared -fPIC -o "$sym/empty.so" "$sym/empty.c"
  } >"$sym/build.log" 2>&1 || built=0
  if [ "$built" -eq 0 ]; then
    fail "could not build the symbol fixtures:
$(cat "$sym/build.log")"
  else
    expect_pass 'symbols/control' "$Y" "$sym/good.so" "$tok" "$FIX/symbols/control"
    expect_fail 'symbols/planted-unnamespaced-export' 'gltang_leaked' \
      "$Y" "$sym/bad.so" "$tok" "$FIX/symbols/control"
    expect_fail 'symbols/planted-split-symbol' 'split symbol' \
      "$Y" "$sym/split.so" "$tok" "$FIX/symbols/control"
    expect_fail 'symbols/planted-api-not-exported' 'Declared GLTANG_API functions' \
      "$Y" "$sym/other.so" "$tok" "$FIX/symbols/control"
    expect_fail 'symbols/planted-no-api' 'gltang_ctx_make' \
      "$Y" "$sym/good.so" "$tok" "$FIX/symbols/planted-no-api"
    expect_fail 'symbols/planted-no-macros' 'ctx_internal.h' \
      "$Y" "$sym/good.so" "$tok" "$FIX/symbols/planted-no-macros"
    expect_fail 'symbols/planted-bad-guard' 'MY_OWN_GUARD_H' \
      "$Y" "$sym/good.so" "$tok" "$FIX/symbols/planted-bad-guard"
    expect_fail 'symbols/planted-dup-guard' 'sharing an include guard' \
      "$Y" "$sym/good.so" "$tok" "$FIX/symbols/planted-dup-guard"
    expect_fail 'symbols/exports-nothing' 'measuring nothing' \
      "$Y" "$sym/empty.so" "$tok" "$FIX/symbols/control"
    expect_fail 'symbols/no-library' 'measuring nothing' \
      "$Y" "$sym/absent.so" "$tok" "$FIX/symbols/control"
  fi
fi

if [ "$failures" -ne 0 ]; then
  printf 'check-gates: %d of %d checks failed\n' "$failures" "$checks" >&2
  exit 1
fi
printf 'check-gates: all %d checks behaved: each gate fails on its planted defect, passes its control, and fails on an empty population\n' \
  "$checks"
