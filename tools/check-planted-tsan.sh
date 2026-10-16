#!/bin/sh
#
# Planted defect 133 (the language layer's slot read without its lock) is a data
# race, which only ThreadSanitizer can see, so it is not one of the cases
# tools/check-planted.sh builds in the release tree. This builds the TSan test of
# the layer in a copy of the library, applies the patch, and requires the
# threaded layer test to REPORT THE RACE; then takes the patch out, rebuilds, and
# requires the same test to be clean (the control).
#
# Usage: PLANTED_PREFIX=<prefix> PLANTED_LIBDIR=<dir> tools/check-planted-tsan.sh
# The two variables are the ones tools/check-planted.sh reads; `make
# check-planted-tsan` sets them. Linux only.

set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"
PATCH="$ROOT/tests/planted/133-language-layer-unlocked.patch"
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

build() {
  # shellcheck disable=SC2086
  (cd "$WORK" && make "$TA/testHost_context" $PREFIX_ARG -j"$JOBS") >>"$LOG" 2>&1
}

run() {
  (cd "$WORK" && env LD_PRELOAD= LD_LIBRARY_PATH="$WORK/$TA:$LIBDIR" TSAN_OPTIONS="halt_on_error=1:history_size=7" \
    timeout 600 "$TA/testHost_context" --gtest_brief=1 --gtest_filter='LanguageLayer.Threads*' 2>&1)
}

rc=0
cp "$WORK/src/library/language.c" "$WORK/language.c.orig"
if ! (cd "$WORK" && patch -p1 --quiet <"$PATCH") >>"$LOG" 2>&1; then
  printf 'check-planted-tsan: the patch does not apply (see %s)\n' "$LOG" >&2
  exit 2
fi
if ! build; then
  printf '  FAIL: the patched copy did not build (see %s)\n' "$LOG" >&2
  exit 1
fi
out="$(run)"
status=$?
if [ "$status" -ne 0 ] && printf '%s' "$out" | grep -q 'ThreadSanitizer: data race'; then
  printf 'planted 133-language-layer-unlocked\n  caught by ThreadSanitizer (exit %s)\n' "$status"
else
  printf 'planted 133-language-layer-unlocked\n  NOT CAUGHT: the threaded layer test did not report a race with the lock removed (exit %s)\n' "$status" >&2
  rc=1
fi
cp "$WORK/language.c.orig" "$WORK/src/library/language.c"
if ! build; then
  printf '  FAIL: the control did not build (see %s)\n' "$LOG" >&2
  exit 1
fi
out="$(run)"
status=$?
if [ "$status" -eq 0 ] && ! printf '%s' "$out" | grep -q 'ThreadSanitizer'; then
  printf '  control is clean\n'
else
  printf '  CONTROL FAILED: the unpatched threaded layer test under ThreadSanitizer exits %s\n' "$status" >&2
  rc=1
fi
exit $rc
