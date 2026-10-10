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
#   18 the deopt hook does not set the callers' identities   the frame differential, a caller's line after a chain is rebuilt   (testObserver)
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
#   32 a float sum one ulp off with the JIT on   the frame differential's bits channel   (testObserver)
#   33 a subnormal product or quotient flushed to zero with the JIT on   the frame differential's bits channel   (testObserver)
#   34 a zero product that is a NaN with the JIT on   the frame differential's bits channel   (testObserver)
#   (32 to 34 stand for the compiled float of spec-runtime-float stories 6 and 7, which does
#   not exist yet: each is a tier-dependent float result, which is what a compiled operation
#   that is wrong would be. The text of a float is six decimals, so for 32 and 33 the text
#   of every slot, the output and the result are unchanged and only the bits differ.)
#
# Cases 13, 14, 15, 20 and 27 (`--relocate`) are caught only by runtime-heap's relocation
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
#   --quick     the cases that finish in about a minute (03 to 09, 11, 12, 16 to 19, 21 to 26 and 28 to 34; 10 is retired); `make test` runs these
#   --slow      the torture cases (01, 02); `make test-torture` runs these
#   --all       every case (the default); `make check-planted`
#   --relocate  cases 13, 14, 15, 20 and 27, against PLANTED_RELOC_PREFIX and
#               PLANTED_RELOC_LIBDIR (a relocation runtime-heap); `make
#               check-planted-relocate` runs these, and `make test-relocate`
#               calls that
#   --selftest  prove the script itself: a patch that applies to nothing fails
#               it, and a patch that breaks nothing is reported as not caught
# PLANTED_JIT is yes (the default) or no, the JIT= the library is built with. The
# copy is built the same way, in its own tree (release-nojit for no). Cases 08, 09
# and 15 to 34 plant a defect in the JIT, which a JIT=no build does not contain, so
# with no they are SKIPPED, loudly, and counted as skipped and never as caught:
# the summary line gives both numbers, and a run in which nothing was caught
# fails (an all-skipped run proves nothing). With yes every case runs.
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
JIT="${PLANTED_JIT:-yes}"
case "$JIT" in
  yes) TREE=release ;;
  no) TREE=release-nojit ;;
  *) printf 'check-planted: PLANTED_JIT must be yes or no, not %s\n' "$JIT" >&2; exit 2 ;;
esac
# Cases whose defect is in code only a JIT=yes build contains.
JIT_ONLY="08-jit-wrong-tag 09-jit-skipped-fuel 15-push-reads-args-first 16-push-omits-call-fuel 17-depth-test-off-by-one 18-deopt-skips-caller-identities 19-deopt-ignores-failed-rebuild 20-poll-helper-copies-the-guest-frame-back 21-compile-hook-does-not-refuse 22-remembered-exit-counts 23-call-passes-the-entry-flag-as-one 24-discard-destroys-the-code-directly 25-native-record-not-closed 26-native-depth-miscount 27-arguments-not-pinned 28-stale-stack-over-a-nested-activation 29-unwind-status-read-as-ok 30-member-load-by-name-not-charged 31-deep-push-refused 32-float-add-one-ulp-off-in-the-jit-arm 33-float-subnormal-flushed-in-the-jit-arm 34-float-nan-where-a-number-belongs-in-the-jit-arm"

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
    --relocate) MODE=relocate ;;
    --selftest) SELFTEST=1 ;;
    -*) printf 'check-planted: unknown option %s\n' "$arg" >&2; exit 2 ;;
    *) NAMED="$NAMED $arg" ;;
  esac
done

# The cases that need a runtime-heap that moves objects.
RELOCATE_CASES="13-temporaries-through-a-copy 14-array-storage-through-a-copy 15-push-reads-args-first 20-poll-helper-copies-the-guest-frame-back 27-arguments-not-pinned"
# What a relocation case runs under: relocation and torture together (a move at
# every GC point). The "relocation is the instrument" check removes the first.
RELOC_ENV="GRHEAP_TORTURE=1 GRHEAP_RELOCATE=1"
QUICK="03-order-dependent-decide 04-native-never-polls 05-frame-slot-mismatch 06-wrong-operator 07-silent-runner 08-jit-wrong-tag 09-jit-skipped-fuel 11-host-pointer-no-hook 12-skipped-output-capture 16-push-omits-call-fuel 17-depth-test-off-by-one 18-deopt-skips-caller-identities 19-deopt-ignores-failed-rebuild 21-compile-hook-does-not-refuse 22-remembered-exit-counts 23-call-passes-the-entry-flag-as-one 24-discard-destroys-the-code-directly 25-native-record-not-closed 26-native-depth-miscount 28-stale-stack-over-a-nested-activation 29-unwind-status-read-as-ok 30-member-load-by-name-not-charged 31-deep-push-refused 32-float-add-one-ulp-off-in-the-jit-arm 33-float-subnormal-flushed-in-the-jit-arm 34-float-nan-where-a-number-belongs-in-the-jit-arm"
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
    09-*) echo "build/linux/$TREE/apps/testJit" ;;
    15-*|16-*|17-*|19-*|20-*|21-*|22-*|23-*|24-*|31-*) echo "build/linux/$TREE/apps/testJit_calls" ;;
    25-*|26-*|27-*|28-*|29-*|30-*) echo "build/linux/$TREE/apps/testNative_calls" ;;
    18-*) echo "build/linux/$TREE/apps/testObserver" ;;
    11-*|12-*) echo "build/linux/$TREE/apps/testSnapshot" ;;
    04-*) echo "build/linux/$TREE/apps/testNative_gate" ;;
    06-*|07-*) echo "build/linux/$TREE/apps/oracle/oracle_ctang build/linux/$TREE/apps/testOracle" ;;
    13-*|14-*) echo "build/linux/$TREE/apps/testExecute_simple" ;;
    selftest) echo "build/linux/$TREE/apps/testObserver" ;;
  esac
}

run_test() {
  case "$1" in
    01-*|02-*)
      (cd "$WORK" && env GRHEAP_TORTURE=1 GRHEAP_VERIFY=1 LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testExecute_complex --gtest_brief=1) ;;
    03-*|05-*|selftest)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testObserver --gtest_brief=1) ;;
    08-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testObserver --gtest_brief=1 --gtest_filter='Observer.PlainTorture*') ;;
    09-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testJit --gtest_brief=1 --gtest_filter='Jit.FuelIsTheSame*') ;;
    32-*|33-*|34-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testObserver --gtest_brief=1 --gtest_filter='Observer.TheFloatPrograms*') ;;
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
    11-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testSnapshot --gtest_brief=1 --gtest_filter='Snapshot.NoHostAddress*') ;;
    12-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testSnapshot --gtest_brief=1 --gtest_filter='Snapshot.OutputSoFar*:SnapshotCorpus.EveryProgram*') ;;
    04-*)
      (cd "$WORK" && env LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testNative_gate --gtest_brief=1) ;;
    13-*|14-*)
      # shellcheck disable=SC2086
      (cd "$WORK" && env -u GRHEAP_TORTURE -u GRHEAP_RELOCATE $RELOC_ENV LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testExecute_simple --gtest_brief=1) ;;
    06-*|07-*)
      (cd "$WORK" && env GLTANG_ORACLE_RUNNER="$APPS/oracle/oracle_ctang" LD_LIBRARY_PATH="$LDPATH" timeout 170 ./build/linux/$TREE/apps/testOracle --gtest_brief=1) ;;
  esac
}

name_of_test() {
  case "$1" in
    01-*|02-*) echo "testExecute_complex under torture and verify" ;;
    03-*|05-*|selftest) echo "testObserver" ;;
    08-*) echo "testObserver (the frame differential, interpreter against JIT)" ;;
    32-*|33-*|34-*) echo "testObserver (the float programs, compared by bits, interpreter against JIT)" ;;
    09-*) echo "testJit (the fuel-parity test)" ;;
    15-*|20-*) echo "testJit_calls under relocation torture" ;;
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
        13-*|14-*|15-*|20-*|27-*)
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
