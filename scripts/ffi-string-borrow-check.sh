#!/bin/sh
# Explicit expected values; this check does not run another Ruby implementation.
set -eu
cd "$(dirname "$0")/.."
SPINEL=${SPINEL:-./bin/spinel}
CC=${CC:-clang}
borrow_tmp=$(mktemp -d "${TMPDIR:-/tmp}/spinel-ffi-borrow.XXXXXX")
trap 'rm -rf "$borrow_tmp"' EXIT HUP INT TERM
pass=0
for fixture in ffi_str_inout ffi_str_inout_edges ffi_str_inout_order ffi_str_inout_cache ffi_str_inout_ivar_alias ffi_str_inout_dead; do
  "$SPINEL" "test/$fixture.rb" --cc="$CC" -o "$borrow_tmp/$fixture"
  "$borrow_tmp/$fixture" > "$borrow_tmp/$fixture.out" 2> "$borrow_tmp/$fixture.err"
  diff -u "test/$fixture.rb.expected" "$borrow_tmp/$fixture.out"
  test ! -s "$borrow_tmp/$fixture.err"
  pass=$((pass + 1))
done
for fixture in test/ffi-str-inout-refuse/*.rb; do
  if "$SPINEL" "$fixture" -S > "$borrow_tmp/refuse.c" 2> "$borrow_tmp/refuse.err"; then
    echo "FAIL accepted $fixture" >&2; exit 1
  fi
  if [ -s "$borrow_tmp/refuse.c" ] || ! grep -q str_inout "$borrow_tmp/refuse.err" ||
     ! grep -F -q -f "${fixture%.rb}.reason" "$borrow_tmp/refuse.err"; then
    echo "FAIL wrong refusal for $fixture" >&2
    cat "$borrow_tmp/refuse.err" >&2
    exit 1
  fi
  pass=$((pass + 1))
done
printf 'ffi-string-borrow: %s pass, 0 fail\n' "$pass"
