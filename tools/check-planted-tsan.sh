#!/bin/sh
#
# Planted defects 133 (the language layer's slot read without its lock) and 268
# (a lookup in a sealed type registry that writes it) are data races, which only
# ThreadSanitizer can see, so they are not among the cases tools/check-planted.sh
# builds in the release tree. For each, this builds the TSan test in a copy of the
# library, applies the patch, and requires the threaded test to REPORT THE RACE;
# then takes the patch out, rebuilds, and requires the same test to be clean (the
# control).
#
# Usage: PLANTED_PREFIX=<prefix> PLANTED_LIBDIR=<dir> tools/check-planted-tsan.sh
# The two variables are the ones tools/check-planted.sh reads; `make
# check-planted-tsan` sets them. Linux only.

set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"
WORK="$ROOT/build/planted-tsan/copy"
LOG="$ROOT/build/planted-tsan/build.log"
JOBS="${PLANTED_JOBS:-8}"
PREFIX="${PLANTED_PREFIX:-}"
LIBDIR="${PLANTED_LIBDIR:-}"

if [ -z "$LIBDIR" ]; then
  printf 'check-planted-tsan: set PLANTED_LIBDIR and PLANTED_PREFIX (make check-planted-tsan does both)\n' >&2
  exit 2
fi
PREFIX_ARG=""
if [ -n "$PREFIX" ]; then PREFIX_ARG="PREFIX=$PREFIX"; fi

rm -rf "$ROOT/build/planted-tsan"
mkdir -p "$WORK"
: >"$LOG"
(cd "$ROOT" && tar --exclude=./build -cf - Makefile bison flex include src tests tools pkgconfig documentation examples bench 2>/dev/null) | tar -C "$WORK" -xf -
if [ ! -f "$WORK/Makefile" ]; then
  printf 'check-planted-tsan: could not make the copy under %s\n' "$WORK" >&2
  exit 2
fi

# Where this build puts its TSan programs, from the Makefile itself.
# shellcheck disable=SC2086
TA="$(cd "$WORK" && make -s -pn $PREFIX_ARG 2>/dev/null | sed -n 's/^TSAN_APP_DIR := //p' | head -1)"
if [ -z "$TA" ]; then
  printf 'check-planted-tsan: the Makefile names no TSAN_APP_DIR\n' >&2
  exit 2
fi

# How many tests a run reports having run, from gtest's "[==========] N tests from ... ran." line;
# 0 when there is none. A filter that matches no test must not read as a clean control.
tests_run() {
  printf '%s\n' "$1" | sed -n 's/^\[==========\] \([0-9][0-9]*\) tests\{0,1\} from .* ran\..*/\1/p' | tail -1 | awk '{print ($1 == "" ? 0 : $1)}'
}

# plant NAME PATCH SOURCE-FILE TEST FILTER
# A failure of one defect (a patch that does not apply, a build that fails) is counted in rc and
# the next defect is still tried.
plant() {
  name="$1"; patch_file="$ROOT/tests/planted/$2"; source_file="$3"; test_name="$4"; filter="$5"
  build() {
    # shellcheck disable=SC2086
    (cd "$WORK" && make "$TA/$test_name" $PREFIX_ARG -j"$JOBS") >>"$LOG" 2>&1
  }
  run() {
    (cd "$WORK" && env LD_PRELOAD= LD_LIBRARY_PATH="$WORK/$TA:$LIBDIR" TSAN_OPTIONS="halt_on_error=1:history_size=7" \
      timeout 600 "$TA/$test_name" --gtest_brief=1 --gtest_filter="$filter" 2>&1)
  }
  cp "$WORK/$source_file" "$WORK/original.orig"
  if ! (cd "$WORK" && patch -p1 --fuzz=0 --quiet <"$patch_file") >>"$LOG" 2>&1; then
    printf 'planted %s\n  FAIL: the patch does not apply (see %s)\n' "$name" "$LOG" >&2
    rc=1
    return
  fi
  if ! build; then
    printf 'planted %s\n  FAIL: the patched copy did not build (see %s)\n' "$name" "$LOG" >&2
    rc=1
    cp "$WORK/original.orig" "$WORK/$source_file"
    return
  fi
  out="$(run)"
  status=$?
  # A reported race stops the run before gtest's summary (halt_on_error), so the race is looked for first.
  if [ "$status" -ne 0 ] && printf '%s' "$out" | grep -q 'ThreadSanitizer: data race'; then
    printf 'planted %s\n  caught by ThreadSanitizer (exit %s)\n' "$name" "$status"
  elif [ "$(tests_run "$out")" -lt 1 ]; then
    printf 'planted %s\n  FAIL: the filter %s ran no test, so nothing was measured\n' "$name" "$filter" >&2
    rc=1
  else
    printf 'planted %s\n  NOT CAUGHT: the threaded test did not report a race with the defect planted (exit %s)\n' "$name" "$status" >&2
    rc=1
  fi
  cp "$WORK/original.orig" "$WORK/$source_file"
  if ! build; then
    printf '  FAIL: the control did not build (see %s)\n' "$LOG" >&2
    rc=1
    return
  fi
  out="$(run)"
  status=$?
  if [ "$(tests_run "$out")" -lt 1 ]; then
    printf '  CONTROL FAILED: the filter %s ran no test\n' "$filter" >&2
    rc=1
  elif [ "$status" -eq 0 ] && ! printf '%s' "$out" | grep -q 'ThreadSanitizer'; then
    printf '  control is clean\n'
  else
    printf '  CONTROL FAILED: the unpatched threaded test under ThreadSanitizer exits %s\n' "$status" >&2
    rc=1
  fi
}

rc=0
plant 133-language-layer-unlocked 133-language-layer-unlocked.patch src/library/language.c testHost_context 'LanguageLayer.Threads*'
plant 268-registry-lookup-writes 268-registry-lookup-writes.patch src/library/types.c testCustom_ops 'TypeOps.TwoContextsOnTwoThreads*'
exit $rc
