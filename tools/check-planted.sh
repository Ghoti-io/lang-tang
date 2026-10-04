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
#   10 a missed write-back at a poll   the write-back test    (testJit)
#   11 a host pointer left in a type's payload with no hook   the address scan of a snapshot (testSnapshot)
#   12 a skipped output-buffer capture   the output-so-far test and the corpus sweep    (testSnapshot)
#
# The copy is of the working tree (sources, tests, corpus, documentation) and
# nothing in the working tree is modified. Each patch is applied with `patch
# --fuzz=0` and the script checks that it matched and that the file changed, so
# a patch that applies to nothing fails the script instead of reading as a gate
# that held. The copy is removed when the script ends (PLANTED_KEEP=1 keeps it).
#
# Usage: PLANTED_PREFIX=<prefix> PLANTED_LIBDIR=<dir> tools/check-planted.sh [--quick | --slow | --all] [--selftest] [case...]
#   --quick     the cases that finish in about a minute (03 to 12); `make test` runs these
#   --slow      the torture cases (01, 02); `make test-torture` runs these
#   --all       every case (the default); `make check-planted`
#   --selftest  prove the script itself: a patch that applies to nothing fails
#               it, and a patch that breaks nothing is reported as not caught
# PLANTED_PREFIX is the PREFIX the dependencies were installed with (empty for
# none) and PLANTED_LIBDIR the directory their shared libraries are in; the
# pkg-config path is the caller's, as for every make in this library.

set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"
PATCHES="$ROOT/tests/planted"
WORK="$ROOT/build/planted/copy"
LOG="$ROOT/build/planted/build.log"
JOBS="${PLANTED_JOBS:-8}"
PREFIX="${PLANTED_PREFIX:-}"
LIBDIR="${PLANTED_LIBDIR:-}"

if [ -z "$LIBDIR" ]; then
  printf 'check-planted: set PLANTED_LIBDIR to the directory the dependencies'"'"' shared libraries are in, and PLANTED_PREFIX to the PREFIX they were installed with (make check-planted does both)\n' >&2
  exit 2
fi
PREFIX_ARG=""
if [ -n "$PREFIX" ]; then
  PREFIX_ARG="PREFIX=$PREFIX"
fi

MODE=all
SELFTEST=0
NAMED=""
for arg in "$@"; do
  case "$arg" in
    --quick) MODE=quick ;;
    --slow) MODE=slow ;;
    --all) MODE=all ;;
    --selftest) SELFTEST=1 ;;
    -*) printf 'check-planted: unknown option %s\n' "$arg" >&2; exit 2 ;;
    *) NAMED="$NAMED $arg" ;;
  esac
done

QUICK="03-order-dependent-decide 04-native-never-polls 05-frame-slot-mismatch 06-wrong-operator 07-silent-runner 08-jit-wrong-tag 09-jit-skipped-fuel 10-jit-missed-write-back 11-host-pointer-no-hook 12-skipped-output-capture"
SLOW="01-missing-root 02-missing-gc-store"

cleanup() {
  if [ "${PLANTED_KEEP:-0}" != 1 ]; then
    rm -rf "$ROOT/build/planted"
  fi
}
trap cleanup EXIT INT TERM

# The test that must notice each patch: the make target that builds it (or the
# runner script for the oracle), and the command that runs it in the copy.
APPS="$WORK/build/linux/release/apps"
LDPATH="$APPS:$LIBDIR"

target_of() {
  case "$1" in
    01-*|02-*) echo "build/linux/release/apps/testExecute_complex" ;;
    03-*|05-*|08-*) echo "build/linux/release/apps/testObserver" ;;
    09-*|10-*) echo "build/linux/release/apps/testJit" ;;
    11-*|12-*) echo "build/linux/release/apps/testSnapshot" ;;
    04-*) echo "build/linux/release/apps/testNative_gate" ;;
    06-*|07-*) echo "build/linux/release/apps/oracle/oracle_ctang build/linux/release/apps/testOracle" ;;
    selftest) echo "build/linux/release/apps/testObserver" ;;
  esac
}

run_test() {
  case "$1" in
    01-*|02-*)
      (cd "$WORK" && env GRHEAP_TORTURE=1 GRHEAP_VERIFY=1 LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/release/apps/testExecute_complex --gtest_brief=1) ;;
    03-*|05-*|selftest)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/release/apps/testObserver --gtest_brief=1) ;;
    08-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/release/apps/testObserver --gtest_brief=1 --gtest_filter='Observer.PlainTorture*') ;;
    09-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/release/apps/testJit --gtest_brief=1 --gtest_filter='Jit.FuelIsTheSame*') ;;
    10-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/release/apps/testJit --gtest_brief=1 --gtest_filter='Jit.AMissedWriteBack*') ;;
    11-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/release/apps/testSnapshot --gtest_brief=1 --gtest_filter='Snapshot.NoHostAddress*') ;;
    12-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/release/apps/testSnapshot --gtest_brief=1 --gtest_filter='Snapshot.OutputSoFar*:SnapshotCorpus.EveryProgram*') ;;
    04-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/release/apps/testNative_gate --gtest_brief=1) ;;
    06-*|07-*)
      (cd "$WORK" && env GLTANG_ORACLE_RUNNER="$APPS/oracle/oracle_ctang" LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/release/apps/testOracle --gtest_brief=1) ;;
  esac
}

name_of_test() {
  case "$1" in
    01-*|02-*) echo "testExecute_complex under torture and verify" ;;
    03-*|05-*|selftest) echo "testObserver" ;;
    08-*) echo "testObserver (the frame differential, interpreter against JIT)" ;;
    09-*) echo "testJit (the fuel-parity test)" ;;
    10-*) echo "testJit (the write-back test)" ;;
    04-*) echo "testNative_gate" ;;
    11-*) echo "testSnapshot (the address scan)" ;;
    12-*) echo "testSnapshot (the output-so-far test and the corpus sweep)" ;;
    06-*|07-*) echo "testOracle" ;;
  esac
}

build() {
  # Incremental: only what the patch touched is rebuilt. The environment's make
  # flags are not this make's.
  (cd "$WORK" && env -u MAKEFLAGS -u MFLAGS -u MAKELEVEL make -j"$JOBS" $PREFIX_ARG $(target_of "$1") >>"$LOG" 2>&1)
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
esac
if [ -n "$NAMED" ]; then
  CASES="$NAMED"
fi

make_copy
count=0
for c in $CASES; do
  patch_file="$PATCHES/$c.patch"
  if [ ! -f "$patch_file" ]; then
    printf 'check-planted: no patch %s\n' "$patch_file" >&2
    exit 2
  fi
  count=$((count + 1))
  if ! run_case "$c" "$patch_file"; then
    failures=$((failures + 1))
  fi
done

if [ "$count" -eq 0 ]; then
  printf 'check-planted: no case ran, so this measures nothing\n' >&2
  exit 1
fi
if [ "$failures" -ne 0 ]; then
  printf 'check-planted: %s of %s planted defects were not caught by their instrument, or their control failed\n' "$failures" "$count" >&2
  exit 1
fi
printf 'check-planted: all %s planted defects were caught by the instrument named for them, and each control passed\n' "$count"
