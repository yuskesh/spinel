#!/bin/sh
# Direct negative controls for the arena allocators' overflow guards.
#
# These are called from C, not from Ruby, on purpose. Every guard here protects
# an addition that can only be reached with a size within a few bytes of
# SIZE_MAX, and no Ruby program can ask for one: the Ruby-level length
# computation overflows or raises first, so a Ruby-driven test would exercise
# nothing and pass whether or not the guard exists. Calling the runtime entry
# points directly is the only way to put the argument where the guard lives.
#
# What each case must do: refuse (non-zero exit, a diagnostic), NOT return a
# small allocation for an enormous request. The "wraps to" column in the output
# is what the unguarded code would have handed back.
#
# usage: scripts/arena-alloc-guards.sh [--cc CMD]
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/.." && pwd)
cd "$root"
CC_="${CC:-cc}"
while [ $# -gt 0 ]; do
  case "$1" in
    --cc) CC_=$2; shift 2 ;;
    --cc=*) CC_=${1#--cc=}; shift ;;
    *) echo "usage: $0 [--cc CMD]" >&2; exit 2 ;;
  esac
done
[ -f lib/libspinel_rt_arena.a ] || { echo "arena-alloc-guards: run make arena-archive first" >&2; exit 2; }

work=$(mktemp -d "${TMPDIR:-/tmp}/arena-guards.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM

cat > "$work/h.c" <<'C'
#include "spinel_rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* spinel_rt.h expects the GENERATED translation unit to supply the program's
   symbol and class tables; a hand-written TU has to stand in for them. None of
   these is reached: every case below dies inside an allocator. */
const char *sp_sym_to_s(sp_sym id) { (void)id; return "?"; }
sp_sym sp_sym_intern(const char *s) { (void)s; return (sp_sym)0; }
const char *sp_class_to_s(sp_Class c) { (void)c; return "?"; }

/* One entry per invocation, so a guard that exits takes only its own case
   down and the harness can see which one it was. */
int main(int argc, char **argv) {
  if (argc != 3) { fprintf(stderr, "usage: h <entry> <slack>\n"); return 2; }
  size_t slack = (size_t)strtoull(argv[2], NULL, 10);
  size_t n = (size_t)-1 - slack;          /* slack bytes below SIZE_MAX */
  void *p = NULL;
  if (!strcmp(argv[1], "gc_alloc"))        p = sp_gc_alloc(n, NULL, NULL);
  else if (!strcmp(argv[1], "gc_alloc_nogc")) p = sp_gc_alloc_nogc(n, NULL, NULL);
  else if (!strcmp(argv[1], "str_alloc"))  p = (void *)sp_str_alloc(n);
  else if (!strcmp(argv[1], "arena_alloc")) p = sp_gc_arena_alloc(n);
  else { fprintf(stderr, "unknown entry %s\n", argv[1]); return 2; }
  /* Reaching here means the request was SATISFIED. That is the failure this
     exists to catch: an allocation of nearly SIZE_MAX bytes cannot have
     succeeded, so the addition wrapped and this pointer is a few bytes long. */
  printf("RETURNED %p for a request of %zu bytes\n", p, n);
  return 0;
}
C

$CC_ -O2 -DSP_PROCESS_ARENA -Ilib "$work/h.c" lib/libspinel_rt_arena.a -lm -lcrypt -o "$work/h" 2>"$work/cc.err" || {
  echo "arena-alloc-guards: harness did not build:" >&2; head -5 "$work/cc.err" >&2; exit 2; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok   %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL %s\n' "$1"; [ $# -lt 2 ] || printf '     %s\n' "$2"; }

# entry, slack (bytes below SIZE_MAX), what the unguarded addition would wrap to
check() { # entry slack label
  _e=$1; _s=$2; _l=$3
  if out=$("$work/h" "$_e" "$_s" 2>&1); then rc=0; else rc=$?; fi
  case "$out" in
    RETURNED*) bad "$_l" "the request was SATISFIED: $out" ;;
    *) if [ "$rc" -ne 0 ]; then ok "$_l  (rc=$rc: $(printf '%s' "$out" | head -1))"
       else bad "$_l" "exited 0 with: $(printf '%s' "$out" | head -1)"; fi ;;
  esac
}

echo "Each request is within a few bytes of SIZE_MAX, so the header/marker"
echo "addition inside the entry would wrap to a tiny allocation."
echo
check gc_alloc      8  "sp_gc_alloc refuses a size that would wrap the header addition"
check gc_alloc_nogc 8  "sp_gc_alloc_nogc refuses a size that would wrap the header addition"
check str_alloc     8  "sp_str_alloc refuses a length that would wrap header+marker+NUL"
check arena_alloc   3  "sp_gc_arena_alloc refuses a size that cannot be 16-byte aligned"

# Not vacuous: a size well below the guards' thresholds must reach the LIMIT
# and be refused there, loudly -- not wrap, and not be quietly satisfied.
if out=$("$work/h" gc_alloc 68719476736 2>&1); then rc=0; else rc=$?; fi
case "$out" in
  RETURNED*) bad "a large-but-representable request is refused by the limit, not satisfied" "$out" ;;
  *spinel:*arena*) [ "$rc" -ne 0 ] && ok "a large-but-representable request is refused by the LIMIT, with the limit diagnostic" \
                     || bad "a large-but-representable request is refused by the limit" "rc=0" ;;
  *) bad "a large-but-representable request is refused by the limit, with the limit diagnostic" "rc=$rc $(printf '%s' "$out" | head -1)" ;;
esac

printf '\narena allocator guards: %s passed, %s failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
