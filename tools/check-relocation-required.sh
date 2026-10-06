#!/bin/sh
#
# Fail, and never skip, when the runtime-heap a prefix holds does not have the
# relocation torture.
#
# The relocation arm (`make test-relocate`) runs this library's suites against a
# runtime-heap built with RELOCATE=yes, where every collection moves every
# unpinned object and poisons the old cell. Pointed at a normal heap, which
# never moves anything, every suite would pass for the wrong reason, so the arm
# starts here and a heap without the mode is an error, as a missing JIT backend
# is for the JIT arm (check-backend-required).
#
# The probe is a small C program linked with what pkg-config gives for the
# prefix. It asks the installed library whether the mode is there
# (grheap_relocation_available), and then shows it working: with
# GRHEAP_RELOCATE=1 a heap that collects once must have moved the object that a
# root holds, and the root must name the new address.
#
# Usage: check-relocation-required.sh <prefix> [branch]
#   <prefix> holds share/pkgconfig and the installed libraries. [branch] is the
#   package suffix, -0 by default.

set -u

PREFIX="${1:?usage: check-relocation-required.sh <prefix> [branch]}"
BRANCH="${2:--0}"
CC="${CC:-cc}"

PKG_CONFIG_PATH="$PREFIX/share/pkgconfig"
export PKG_CONFIG_PATH
modules="ghoti.io-runtime-heap$BRANCH ghoti.io-runtime-core$BRANCH ghoti.io-cutil$BRANCH"
# shellcheck disable=SC2086
flags="$(pkg-config --cflags --libs $modules 2>/dev/null)"
if [ -z "$flags" ]; then
  printf 'check-relocation-required: pkg-config finds no runtime-heap under %s\n' "$PREFIX" >&2
  exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT INT TERM

cat > "$work/probe.c" <<'EOC'
#include <ghoti.io/runtime-heap/runtime-heap.h>
#if defined(__has_include)
#if __has_include(<ghoti.io/runtime-heap/relocate.h>)
#include <ghoti.io/runtime-heap/relocate.h>
#define HAVE_RELOCATE_H 1
#endif
#endif

#include <ghoti.io/runtime-core/b/context.h>
#include <ghoti.io/runtime-core/b/group.h>

#include <stdint.h>
#include <stdio.h>

static const GRHEAP_Type leaf = GRHEAP_TYPE_INIT("leaf", 32, NULL, NULL, NULL, NULL, NULL);
static void * root;

int main(void) {
  if (!grheap_relocation_available()) {
    fprintf(stderr, "the runtime-heap it links says it was built without RELOCATE=yes\n");
    return 3;
  }
#ifndef HAVE_RELOCATE_H
  fprintf(stderr, "the runtime-heap says it has relocation but installed no relocate.h\n");
  return 4;
#else
  GRCORE_Group * group;
  GRCORE_Context * context;
  GRHEAP_Heap * heap;
  if (grcore_group_create(NULL, NULL, &group) != GRCORE_OK ||
      grcore_context_create(group, NULL, &context) != GRCORE_OK ||
      grheap_heap_create(context, NULL, &heap) != GRHEAP_OK) {
    fprintf(stderr, "the probe could not make a heap\n");
    return 5;
  }
  /* A pinned neighbour keeps the block mapped; then one object, rooted. */
  void * keeper;
  void * object;
  if (grheap_alloc(heap, &leaf, &keeper) != GRHEAP_OK || grheap_pin(heap, keeper) != GRHEAP_OK ||
      grheap_alloc(heap, &leaf, &object) != GRHEAP_OK) {
    return 5;
  }
  root = object;
  if (grheap_root_add(heap, &root) != GRHEAP_OK || grheap_collect(heap) != GRHEAP_OK) {
    return 5;
  }
  GRHEAP_RelocationStats stats;
  if (grheap_relocation_stats(heap, &stats) != GRHEAP_OK) {
    return 5;
  }
  if (stats.moved == 0 || root == object) {
    fprintf(stderr, "GRHEAP_RELOCATE=1 did not make a collection move the object (moved %llu): the arm would prove nothing\n",
        (unsigned long long)stats.moved);
    return 6;
  }
  (void)grcore_context_destroy(context);
  (void)grcore_group_destroy(group);
  return 0;
#endif
}
EOC

# shellcheck disable=SC2086
if ! $CC -std=c17 -Wall -Wextra -o "$work/probe" "$work/probe.c" $flags >"$work/build.log" 2>&1; then
  printf 'check-relocation-required: the probe did not build against %s:\n%s\n' "$PREFIX" "$(cat "$work/build.log")" >&2
  exit 1
fi
libdirs=""
for tok in $flags; do
  case "$tok" in
    -L*) libdirs="${libdirs:+$libdirs:}${tok#-L}" ;;
  esac
done
out="$(env GRHEAP_RELOCATE=1 LD_LIBRARY_PATH="$libdirs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "$work/probe" 2>&1)"
rc=$?
if [ "$rc" -ne 0 ]; then
  printf 'check-relocation-required: the runtime-heap under %s is not a relocation build: %s\n' "$PREFIX" "$out" >&2
  printf '  build it with `make install RELOCATE=yes PREFIX=<a prefix of its own>` in libs/runtime-heap, and pass that as RELOCATE_PREFIX\n' >&2
  exit 1
fi
printf 'check-relocation-required: the runtime-heap under %s has the relocation torture, and a collection moved the probe object\n' "$PREFIX"
