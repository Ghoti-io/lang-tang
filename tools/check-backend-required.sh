#!/bin/sh
#
# Show that the tier-up tests FAIL, and do not skip, when a target that has a
# native code backend has none.
#
# tests/unit/test_jit.cpp starts every tier-up test with
# GLTANG_REQUIRE_JIT_BACKEND(). On Linux x86-64, Linux arm64 and Windows x86-64
# that macro asserts the backend exists; elsewhere it skips, by name. A macro
# that quietly skipped everywhere would turn all of the tests into passes over
# nothing, and a clean run cannot tell the two apart. So this runs the suite
# twice:
#
#   control   as built: it must pass
#   planted   with GLTANG_TEST_FORCE_NO_BACKEND=1, which makes
#             jit_backend_present() answer false as a missing backend would:
#             it must exit non-zero, and every test that uses the macro must
#             be among the failures
#
# On a target without a backend the planted run must instead skip every one of
# them and exit zero, and the script says so by name.
#
# Usage: tools/check-backend-required.sh <testJit executable> <test source>
# LD_LIBRARY_PATH is the caller's.

set -u

exe="$1"
src="$2"

if [ ! -x "$exe" ]; then
  printf 'check-backend-required: %s is not built\n' "$exe" >&2
  exit 1
fi

# The number of tests that use the macro, counted in the source, so that a test
# added without it, or the macro removed from one, changes what is expected.
want="$(grep -c '^  GLTANG_REQUIRE_JIT_BACKEND();' "$src")"
if [ "$want" -lt 1 ]; then
  printf 'check-backend-required: no test in %s uses GLTANG_REQUIRE_JIT_BACKEND, so this checks nothing\n' "$src" >&2
  exit 1
fi

# Whether this target is one with a backend: the same three targets as
# GLTANG_TEST_BACKEND_GATED in tests/test_helpers.h.
case "$(uname -s)-$(uname -m)" in
  Linux-x86_64|Linux-aarch64|MINGW64*-x86_64|MSYS*-x86_64) gated=yes ;;
  *) gated=no ;;
esac

out="$("$exe" --gtest_brief=1 2>&1)"
rc=$?
if [ "$rc" -ne 0 ]; then
  printf 'check-backend-required: the control run (backend as built) failed:\n%s\n' "$out" >&2
  exit 1
fi

out="$(GLTANG_TEST_FORCE_NO_BACKEND=1 "$exe" --gtest_brief=1 2>&1)"
rc=$?
if [ "$gated" = yes ]; then
  failed="$(printf '%s\n' "$out" | sed -n 's/^\[  FAILED  \] \(Jit\.[^ ,]*\).*/\1/p' | sort -u | wc -l)"
  if [ "$rc" -eq 0 ]; then
    printf 'check-backend-required: with the backend forced off the tier-up tests PASSED (or skipped): they cannot fail\n%s\n' "$out" >&2
    exit 1
  fi
  if [ "$failed" -lt "$want" ]; then
    printf 'check-backend-required: with the backend forced off %s tests failed, but %s use GLTANG_REQUIRE_JIT_BACKEND\n%s\n' "$failed" "$want" "$out" >&2
    exit 1
  fi
  printf 'check-backend-required: ok: backend forced off, %s of %s tier-up tests fail; as built, the suite passes\n' "$failed" "$want"
else
  if [ "$rc" -ne 0 ]; then
    printf 'check-backend-required: on a target without a backend the forced run must skip, and it failed:\n%s\n' "$out" >&2
    exit 1
  fi
  printf 'check-backend-required: this target has no native code backend, so the tier-up tests skip, by name (nothing to fail)\n'
fi
