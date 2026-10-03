#!/bin/sh
#
# Fail if a public header carries no stability label, more than one, or the
# wrong one.
#
# AD-14: every public header says whether it is `stable` (frozen at the major
# version once released) or `free` (a consumer requires the exact version it
# was built against). The label is a Doxygen tag in the header's @file block:
#
#     @stability stable
#
# Which headers are which is decided here, by name, so that neither the author
# of a header nor a quiet edit can move one across the line:
#
#   stable   the C embedding API: core.h, parse.h, libver.h, macros.h,
#            namespace.h, allocator.h, seeds.h, and the umbrella lang-tang.h.
#            seeds.h is the one host header that includes only stable ones.
#   free     the syntax tree's node classes (everything under ast/) and the
#            two headers they are built on, location.h and unicodeString.h,
#            and tangScanner.h, which the generated scanner and parser share.
#            The compiler reads these, so their shape may change.
#   free     also the engine's: bytecode.h, program.h, compile.h, value.h,
#            execution.h and library.h (a library holds templates, which are
#            programs, and native functions that name error kinds; both are
#            free, and a stable header may include only stable ones). They
#            are new and unproven; a header joins the stable set by a decision
#            to freeze it, taken here.
#
# A header in neither list fails: a new header is a decision about its
# stability, and it is taken by adding it to a list on purpose.
#
# Usage: check-labels.sh <root>
#   <root> holds include/ghoti.io/lang-tang/. The self-test runs this same
#   script against tests/gates fixtures.
#
# Fails on an empty population: no headers found means the gate measured
# nothing, and a gate that measures nothing reports success.

set -eu

ROOT="${1:?usage: check-labels.sh <root>}"
BASE="$ROOT/include/ghoti.io/lang-tang"

STABLE='core.h parse.h libver.h macros.h namespace.h allocator.h seeds.h lang-tang.h'
FREE='location.h unicodeString.h tangScanner.h bytecode.h program.h compile.h value.h execution.h library.h'

if [ ! -d "$BASE" ]; then
  printf 'check-labels: %s does not exist; this gate is measuring nothing\n' \
    "$BASE" >&2
  exit 1
fi

headers="$(find "$BASE" -type f -name '*.h' | sort)"
if [ -z "$headers" ]; then
  printf 'check-labels: no headers under %s; this gate is measuring nothing\n' \
    "$BASE" >&2
  exit 1
fi

status=0
count=0
for h in $headers; do
  count=$((count + 1))
  rel="${h#"$BASE"/}"
  want=""
  case "$rel" in
    ast/*) want=free ;;
    */*) want="" ;;
    *)
      for name in $STABLE; do
        [ "$rel" = "$name" ] && want=stable
      done
      for name in $FREE; do
        [ "$rel" = "$name" ] && want=free
      done
      ;;
  esac
  if [ -z "$want" ]; then
    printf 'check-labels: %s is in neither the stable nor the free list of tools/check-labels.sh; classify it on purpose\n' \
      "$h" >&2
    status=1
    continue
  fi
  # Anchored to a comment line so that prose mentioning the tag does not count
  # as carrying it.
  labels="$(grep -E '^[[:space:]]*(\*|//)[[:space:]]*@stability[[:space:]]' "$h" \
    | sed 's/.*@stability[[:space:]]*//; s/[[:space:]]*$//' || true)"
  if [ -z "$labels" ]; then
    printf 'check-labels: %s has no @stability label (want %s)\n' \
      "$h" "$want" >&2
    status=1
    continue
  fi
  if [ "$(printf '%s\n' "$labels" | wc -l)" -ne 1 ]; then
    printf 'check-labels: %s has more than one @stability label\n' "$h" >&2
    status=1
    continue
  fi
  case "$labels" in
    stable | free) ;;
    *)
      printf 'check-labels: %s: "%s" is not stable or free\n' "$h" "$labels" >&2
      status=1
      continue
      ;;
  esac
  if [ "$labels" != "$want" ]; then
    printf 'check-labels: %s is labelled %s but must be %s\n' \
      "$h" "$labels" "$want" >&2
    status=1
  fi
done

if [ "$status" -ne 0 ]; then
  exit 1
fi
printf 'check-labels: %d public headers, each labelled as tools/check-labels.sh says (stable: %s; free: ast/ and %s)\n' \
  "$count" "$STABLE" "$FREE"
