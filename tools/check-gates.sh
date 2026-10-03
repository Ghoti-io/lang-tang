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
expect_fail 'labels/planted-unclassified' 'newthing.h is in neither' \
  "$L" "$FIX/labels/planted-unclassified"
expect_fail 'labels/empty' 'measuring nothing' "$L" "$work/empty"

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
expect_fail 'edges/planted-jit' 'lang-tang -> runtime-jit' "$E" --includes "$FIX/edges/planted-jit"
expect_fail 'edges/planted-engine' 'lang-tang -> lang-wasm' "$E" --includes "$FIX/edges/planted-engine"
expect_fail 'edges/planted-binary-h' 'binary.h' "$E" --includes "$FIX/edges/planted-binary-h"
expect_fail 'edges/planted-binary-h-quoted' 'binary.h' "$E" --includes "$FIX/edges/planted-binary-h-quoted"
expect_fail 'edges/planted-binary-h-ctang' 'binary.h' "$E" --includes "$FIX/edges/planted-binary-h-ctang"
expect_fail 'edges/planted-no-include-dir (a population that is missing half)' 'measuring nothing' \
  "$E" --includes "$FIX/edges/planted-no-include-dir"
expect_fail 'edges/empty (includes)' 'measuring nothing' "$E" --includes "$work/empty"

printf 'check-edges --links\n'
# TODO(windows): the .dll arm has not been run; objdump -p is the reader there.
case "$(uname -s)" in
  MINGW* | MSYS*) SHEXT=dll; SHFLAGS="-shared" ;;
  Darwin) SHEXT=dylib; SHFLAGS="-dynamiclib" ;;
  *) SHEXT=so; SHFLAGS="-shared -fPIC" ;;
esac
stubs="$work/stubs"
mkdir -p "$stubs" "$work/planted-tang" "$work/planted-debug" "$work/planted-jit" "$work/control" "$work/dev" "$work/exe-planted" "$work/exe-control"
printf 'int stub_tang(void) { return 1; }\n' > "$work/tang.c"
printf 'int stub_debug(void) { return 2; }\n' > "$work/debug.c"
printf 'int stub_jit(void) { return 3; }\n' > "$work/jit.c"
printf 'int stub_cutil(void) { return 4; }\n' > "$work/cutil.c"
printf 'int stub_unicode(void) { return 5; }\n' > "$work/unicode.c"
printf 'int stub_self(void) { return 6; }\n' > "$work/self.c"
printf 'int stub_tang(void);\nint planted_tang(void) { return stub_tang(); }\n' > "$work/planted_tang.c"
printf 'int stub_debug(void);\nint planted_debug(void) { return stub_debug(); }\n' > "$work/planted_debug.c"
printf 'int stub_jit(void);\nint planted_jit(void) { return stub_jit(); }\n' > "$work/planted_jit.c"
printf 'int stub_cutil(void);\nint stub_unicode(void);\nint control(void) { return stub_cutil() + stub_unicode(); }\n' > "$work/control.c"
printf 'int stub_cutil(void);\nint dev(void) { return stub_cutil(); }\n' > "$work/dev.c"
printf 'int stub_tang(void);\nint stub_self(void);\nint main(void) { return stub_tang() + stub_self(); }\n' > "$work/exe_planted.c"
printf 'int stub_cutil(void);\nint stub_unicode(void);\nint stub_self(void);\nint main(void) { return stub_cutil() + stub_unicode() + stub_self(); }\n' > "$work/exe_control.c"

built=1
# shellcheck disable=SC2086
{
  $CC $SHFLAGS -o "$stubs/libghoti.io-tang-0.$SHEXT" "$work/tang.c" &&
  $CC $SHFLAGS -o "$stubs/libghoti.io-runtime-debug-0.$SHEXT" "$work/debug.c" &&
  $CC $SHFLAGS -o "$stubs/libghoti.io-runtime-jit-0.$SHEXT" "$work/jit.c" &&
  $CC $SHFLAGS -o "$stubs/libghoti.io-cutil-0.$SHEXT" "$work/cutil.c" &&
  $CC $SHFLAGS -o "$stubs/libghoti.io-unicode-0.$SHEXT" "$work/unicode.c" &&
  $CC $SHFLAGS -o "$stubs/libghoti.io-lang-tang-0.$SHEXT" "$work/self.c" &&
  $CC $SHFLAGS -o "$stubs/libghoti.io-cutil-dev.$SHEXT" "$work/cutil.c" &&
  $CC $SHFLAGS -o "$work/planted-tang/libplanted.$SHEXT" "$work/planted_tang.c" \
    -L"$stubs" -Wl,--no-as-needed -l:libghoti.io-tang-0.$SHEXT &&
  $CC $SHFLAGS -o "$work/planted-debug/libplanted.$SHEXT" "$work/planted_debug.c" \
    -L"$stubs" -Wl,--no-as-needed -l:libghoti.io-runtime-debug-0.$SHEXT &&
  $CC $SHFLAGS -o "$work/planted-jit/libplanted.$SHEXT" "$work/planted_jit.c" \
    -L"$stubs" -Wl,--no-as-needed -l:libghoti.io-runtime-jit-0.$SHEXT &&
  $CC $SHFLAGS -o "$work/control/libcontrol.$SHEXT" "$work/control.c" \
    -L"$stubs" -Wl,--no-as-needed -l:libghoti.io-cutil-0.$SHEXT \
    -l:libghoti.io-unicode-0.$SHEXT &&
  $CC $SHFLAGS -o "$work/dev/libdev.$SHEXT" "$work/dev.c" \
    -L"$stubs" -Wl,--no-as-needed -l:libghoti.io-cutil-dev.$SHEXT &&
  $CC -o "$work/exe-planted/tang" "$work/exe_planted.c" \
    -L"$stubs" -Wl,--no-as-needed -l:libghoti.io-tang-0.$SHEXT \
    -l:libghoti.io-lang-tang-0.$SHEXT &&
  $CC -o "$work/exe-control/tang" "$work/exe_control.c" \
    -L"$stubs" -Wl,--no-as-needed -l:libghoti.io-cutil-0.$SHEXT \
    -l:libghoti.io-unicode-0.$SHEXT -l:libghoti.io-lang-tang-0.$SHEXT
} >"$work/build.log" 2>&1 || built=0
if [ "$built" -eq 0 ]; then
  fail "could not build the link-line fixtures:
$(cat "$work/build.log")"
else
  expect_pass 'links/control (cutil and unicode)' "$E" --links "$work/control"
  expect_pass 'links/cutil with a BRANCH suffix (-dev)' "$E" --links "$work/dev"
  expect_pass 'links/program control (cutil, unicode, lang-tang)' \
    "$E" --links "$work/exe-control/tang"
  expect_fail 'links/planted-tang (a shared object linking ctang)' 'lang-tang -> tang' \
    "$E" --links "$work/planted-tang"
  expect_fail 'links/planted-tang names the object' 'libplanted' \
    "$E" --links "$work/planted-tang"
  expect_fail 'links/planted-debug' 'lang-tang -> runtime-debug' "$E" --links "$work/planted-debug"
  expect_fail 'links/planted-jit' 'lang-tang -> runtime-jit' "$E" --links "$work/planted-jit"
  expect_fail 'links/planted-tang-program (the tang command linking ctang)' 'lang-tang -> tang' \
    "$E" --links "$work/exe-planted/tang"
  expect_fail 'links/planted-tang-program names the program' 'exe-planted/tang' \
    "$E" --links "$work/exe-planted/tang"
fi
expect_fail 'links/empty' 'measuring nothing' "$E" --links "$work/empty"
expect_fail 'links/a path that does not exist' 'does not exist' "$E" --links "$work/no-such-thing"

if [ "$failures" -ne 0 ]; then
  printf 'check-gates: %d of %d checks failed\n' "$failures" "$checks" >&2
  exit 1
fi
printf 'check-gates: all %d checks behaved: each gate fails on its planted defect, passes its control, and fails on an empty population\n' \
  "$checks"
