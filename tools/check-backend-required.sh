#!/bin/sh
#
# Show that the tests that need a native code backend FAIL, and do not skip or
# quietly check less, when a target that has a backend has none.
#
# tests/unit/test_jit.cpp starts every tier-up test with
# GLTANG_REQUIRE_JIT_BACKEND(). On Linux x86-64, Linux arm64 and Windows x86-64
# that macro asserts the backend exists; elsewhere it skips, by name. And
# test_profile, test_retention and test_observer guard their "the JIT arm is not
# vacuous" checks with `if (jit_backend_present())`, after
# GLTANG_EXPECT_JIT_BACKEND_ON_GATED_TARGET(). A clean run cannot tell a macro
# that quietly skips or a guard that quietly switches checks off from the real
# thing, so each suite runs twice:
#
#   control   as built: it must pass
#   planted   with GLTANG_TEST_FORCE_NO_BACKEND=1, which makes
#             jit_backend_present() answer false as a missing backend would:
#             it must exit non-zero
#
# For testJit the set of failed test names must also equal the set of tests that
# use the macro, both read as names (from `TEST(Suite, Name)` and the gtest
# failure lines), so that a test added without the macro, or one that loses it,
# changes what is expected.
#
# On a target without a backend the planted runs must instead exit zero, and the
# script says so by name.
#
# Usage: tools/check-backend-required.sh <apps directory> <tests/unit directory> [<executable extension>]
# The extension is ".exe" on Windows, where a program is not found without it.
# LD_LIBRARY_PATH is the caller's.

set -u

apps="$1"
unit="$2"
ext="${3:-}"

# Whether this target is one with a backend: the same three targets as
# GLTANG_TEST_BACKEND_GATED in tests/test_helpers.h.
case "$(uname -s)-$(uname -m)" in
  Linux-x86_64|Linux-aarch64|MINGW64*-x86_64|MSYS*-x86_64) gated=yes ;;
  *) gated=no ;;
esac

fail() {
  printf 'check-backend-required: %s\n' "$*" >&2
  exit 1
}

# run <executable> <forced>: the output; sets rc.
run() {
  if [ "$2" = yes ]; then
    out="$(GLTANG_TEST_FORCE_NO_BACKEND=1 "$1" --gtest_brief=1 2>&1)"
  else
    out="$("$1" --gtest_brief=1 2>&1)"
  fi
  rc=$?
}

for suite in Jit Profile Retention Observer; do
  exe="$apps/test$suite$ext"
  [ -x "$exe" ] || fail "$exe is not built"
  run "$exe" no
  [ "$rc" -eq 0 ] || fail "the control run of test$suite (backend as built) failed:
$out"
done

if [ "$gated" != yes ]; then
  for suite in Jit Profile Retention Observer; do
    run "$apps/test$suite$ext" yes
    [ "$rc" -eq 0 ] || fail "on a target without a backend the forced run of test$suite must pass or skip, and it failed:
$out"
  done
  printf 'check-backend-required: this target has no native code backend, so the tier-up tests skip, by name (nothing to fail)\n'
  exit 0
fi

# The tests that use the macro: the name of the TEST above each use, one per line.
wanted="$(awk '
  /^TEST\(/ { s = $0; sub(/^TEST\(/, "", s); sub(/\).*/, "", s); gsub(/[ \t]/, "", s); sub(/,/, ".", s); name = s }
  /^[ \t]*GLTANG_REQUIRE_JIT_BACKEND[ \t]*\(\)/ { print name }
' "$unit/test_jit.cpp" | sort -u)"
want_count="$(printf '%s\n' "$wanted" | grep -c .)"
[ "$want_count" -ge 1 ] || fail "no test in $unit/test_jit.cpp uses GLTANG_REQUIRE_JIT_BACKEND, so this checks nothing"

run "$apps/testJit$ext" yes
[ "$rc" -ne 0 ] || fail "with the backend forced off the tier-up tests PASSED (or skipped): they cannot fail
$out"
failed="$(printf '%s\n' "$out" | sed -n 's/^\[ *FAILED *\] *\([A-Za-z0-9_]*\.[A-Za-z0-9_]*\).*/\1/p' | sort -u)"
if [ "$failed" != "$wanted" ]; then
  fail "with the backend forced off, the tests that failed are not the tests that use GLTANG_REQUIRE_JIT_BACKEND
  only in the macro's users:
$(printf '%s\n' "$wanted" | grep -vxF "$failed" | sed 's/^/    /')
  only in the failures:
$(printf '%s\n' "$failed" | grep -vxF "$wanted" | sed 's/^/    /')"
fi

for suite in Profile Retention Observer; do
  run "$apps/test$suite$ext" yes
  [ "$rc" -ne 0 ] || fail "with the backend forced off test$suite passed: its \"JIT arm is not vacuous\" checks switch themselves off instead of failing
$out"
done

printf 'check-backend-required: ok: backend forced off, the %s tier-up tests that use the macro fail by name, and testProfile, testRetention and testObserver each fail; as built, all four pass\n' "$want_count"
