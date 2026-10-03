#!/bin/sh
#
# Fail on a dependency edge the spine forbids, in either place it can hide.
#
# AD-2: lang-tang depends on cutil and unicode (what the ported parser uses),
# and may depend on runtime-core and runtime-heap when execution arrives. It
# never reaches codegen (runtime-jit), the debugger (runtime-debug), or ctang:
# "any engine library -> ctang, except under test/". It also includes nothing
# of ctang's bytecode machinery: nothing is built on `binary.h` (AD-9).
#
# The one exception is the hosts. The library (the shared and the static
# object, everything under src/ and include/ but the `tang` command) never
# names the debugger, or `text`, which only the debugger needs; two programs
# do, because a host is where a debugger is attached and a DAP session is
# served (story 13): src/tang.c (the `tang` command) and examples/web_server.c.
# They may include runtime-debug and text, and only the `tang` binary and the
# web_server example binary may have them in their NEEDED list - and, because
# text requires them, chron and regex with them. The allowance is by file, not
# by directory: an include of the debugger in any other file, a different
# example included, is the edge this gate exists to catch. tests/ is not
# scanned at all (it is where the oracle may include ctang), so a test runner
# may link runtime-debug for its framing helpers; that is a host too, and no
# test binary is named to this gate.
#
# The rule is checked twice because the two checks see different things: a
# manifest and a clean #include list can both be right while the shared object
# still links a forbidden library, and an #include can be forbidden while the
# link line is clean. The link check covers the shared library and the `tang`
# command that links it.
#
# The check is an allowlist, not a list of known-bad names: any
# <ghoti.io/X/...> include or libghoti.io-X NEEDED entry whose X is not in the
# list is an edge. A new forbidden library then needs no edit here, and a typo
# in a deny list cannot let one through. The rule inside the allowlist is the
# `binary.h` one.
#
# Where the includes are looked for: the library (src, include), its generator
# inputs (bison, flex), and the programs that ship with it (bench, examples).
# tests/ is the one place that may include ctang - the oracle runner is there
# on purpose - so it is not scanned, and a fixture shows that an include there
# passes while the same line anywhere else does not.
#
# Usage:
#   check-edges.sh --includes <root>      scan <root>/{src,include,bison,flex,bench,examples}
#   check-edges.sh --links <file|dir>...  read the NEEDED list of each shared
#                                         object or program named, or of every
#                                         shared object under a directory
#
# Each mode fails on an empty population.

set -eu

usage='usage: check-edges.sh --includes <root> | --links <file|dir>...'
mode="${1:?$usage}"
[ $# -ge 2 ] || { printf '%s\n' "$usage" >&2; exit 2; }
shift

ALLOWED='cutil|unicode|runtime-core|runtime-heap|lang-tang'
# What a host may include or link as well. The link set is the include set and
# what text requires (its own NEEDED entries are chron and regex).
HOST_INCLUDES='runtime-debug|text'
HOST_LINKS='runtime-debug|text|chron|regex'
# The hosts, by path under the root for an include and by file name for a
# program. Nothing else is one.
is_host_source() {
  case "$1" in
    src/tang.c | examples/web_server.c) return 0 ;;
  esac
  return 1
}
is_host_program() {
  case "$(basename "$1" .exe)" in
    tang | web_server) return 0 ;;
  esac
  return 1
}
status=0

case "$mode" in
  --includes)
    target="$1"
    files=""
    for d in src include bison flex bench examples; do
      if [ -d "$target/$d" ]; then
        found="$(find "$target/$d" -type f \( -name '*.c' -o -name '*.h' \
          -o -name '*.cpp' -o -name '*.y' -o -name '*.l' \) | sort)"
        files="$files
$found"
      fi
    done
    files="$(printf '%s\n' "$files" | sed '/^$/d')"
    if [ -z "$files" ] || [ ! -d "$target/src" ] || [ ! -d "$target/include" ]; then
      printf 'check-edges: no sources under %s/src and %s/include; this gate is measuring nothing\n' \
        "$target" "$target" >&2
      exit 1
    fi
    count=0
    for f in $files; do
      count=$((count + 1))
      # Nothing is built on ctang's binary.h, however it is spelled.
      binary="$(grep -nE '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"][^>"]*binary\.h[>"]' "$f" || true)"
      if [ -n "$binary" ]; then
        printf 'check-edges: forbidden include of binary.h (AD-9: nothing is built on ctang'"'"'s binary.h): %s: %s\n' \
          "$f" "$binary" >&2
        status=1
      fi
      hits="$(grep -nE '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]ghoti\.io/[^/>"]+/' "$f" || true)"
      [ -n "$hits" ] || continue
      while IFS= read -r line; do
        n="${line%%:*}"
        lib="$(printf '%s\n' "$line" \
          | sed -E 's/.*[<"]ghoti\.io\/([^\/>"]+)\/.*/\1/')"
        if printf '%s\n' "$lib" | grep -qE "^($ALLOWED)\$"; then
          continue
        fi
        if is_host_source "${f#"$target"/}" && printf '%s\n' "$lib" | grep -qE "^($HOST_INCLUDES)\$"; then
          continue
        fi
        printf 'check-edges: forbidden edge lang-tang -> %s: %s:%s: %s\n' \
          "$lib" "$f" "$n" "${line#*:}" >&2
        status=1
      done <<HITS
$hits
HITS
    done
    if [ "$status" -ne 0 ]; then
      exit 1
    fi
    printf 'check-edges: %d source files, every #include of another Ghoti library is cutil, unicode, runtime-core, runtime-heap or this library (the hosts src/tang.c and examples/web_server.c may also include runtime-debug and text), and none is binary.h\n' \
      "$count"
    ;;

  --links)
    objects=""
    for t in "$@"; do
      if [ -d "$t" ]; then
        found="$(find "$t" -type f \( -name '*.so' -o -name '*.so.*' \
          -o -name '*.dll' -o -name '*.dylib' \) 2>/dev/null | sort)"
        objects="$objects
$found"
      elif [ -f "$t" ]; then
        objects="$objects
$t"
      else
        printf 'check-edges: %s does not exist; this gate is measuring nothing\n' "$t" >&2
        exit 1
      fi
    done
    objects="$(printf '%s\n' "$objects" | sed '/^$/d')"
    if [ -z "$objects" ]; then
      printf 'check-edges: no shared objects or programs named; this gate is measuring nothing\n' >&2
      exit 1
    fi
    count=0
    for so in $objects; do
      count=$((count + 1))
      if [ "$(uname -s)" = Darwin ] && command -v otool >/dev/null 2>&1; then
        needed="$(otool -L "$so" | sed '1d' | awk '{print $1}' \
          | sed 's|.*/||')"
      elif command -v readelf >/dev/null 2>&1 \
        && readelf -d "$so" >/dev/null 2>&1; then
        needed="$(readelf -d "$so" \
          | sed -n 's/.*Shared library: \[\(.*\)\].*/\1/p')"
      elif command -v objdump >/dev/null 2>&1; then
        # objdump -p reads both ELF (NEEDED) and PE (DLL Name:), which is what
        # a Windows build has.
        needed="$(objdump -p "$so" \
          | awk '$1 == "NEEDED" {print $2} /DLL Name:/ {print $3}')"
      else
        printf 'check-edges: neither readelf nor objdump is available; cannot read %s\n' \
          "$so" >&2
        exit 1
      fi
      if [ -z "$needed" ]; then
        # A real shared object or program links at least libc. An empty answer
        # is a reader that read nothing, which would pass any planted link.
        printf 'check-edges: read no dependencies from %s; the reader is not seeing the link line\n' \
          "$so" >&2
        exit 1
      fi
      for dep in $needed; do
        case "$dep" in
          libghoti.io-*)
            lib="${dep#libghoti.io-}"
            # libghoti.io-lang-tang-0.so.0 -> lang-tang.
            lib="$(printf '%s\n' "$lib" \
              | sed -E 's/(-[0-9]+)?(-debug)?(\.so.*|\.dll.*|\.dylib.*)$//')"
            # Exactly an allowed name, or one followed by a BRANCH suffix
            # (cutil-dev, cutil-0-debug); never a different library that
            # merely starts with one.
            if printf '%s\n' "$lib" | grep -qE "^($ALLOWED)(-.*)?\$"; then
              continue
            fi
            if is_host_program "$so" && printf '%s\n' "$lib" | grep -qE "^($HOST_LINKS)(-.*)?\$"; then
              continue
            fi
            printf 'check-edges: forbidden edge lang-tang -> %s: %s has NEEDED %s\n' \
              "$lib" "$so" "$dep" >&2
            status=1
            ;;
        esac
      done
    done
    if [ "$status" -ne 0 ]; then
      exit 1
    fi
    printf 'check-edges: %d shared objects or programs, every Ghoti library they link is cutil, unicode, runtime-core, runtime-heap or this library (the tang and web_server programs may also link runtime-debug and text)\n' \
      "$count"
    ;;

  *)
    printf '%s\n' "$usage" >&2
    exit 2
    ;;
esac
