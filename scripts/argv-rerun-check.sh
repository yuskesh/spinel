#!/bin/sh
# A generated main run twice in one process (a host that calls the program's
# main once per request) must not mark the previous run's ARGV slots.
#
# The ARGV fill sets sp_argv.len after each slot it writes, because a copy
# can start a collection, which marks sp_argv.data[0 .. len). The fill starts
# from len 0: without that, the second run's first copy marks the first
# run's count of slots in the second run's fresh block, which holds
# MALLOC_PERTURB_ bytes, and the mark follows them.
#
# Cases:
#   rerun           the program's main, renamed, is called twice under
#                   SPINEL_GC_STRESS=1 MALLOC_PERTURB_=165 with a first
#                   argument over 2,048 bytes; both runs print every length
#   control         the same C with the `sp_argv.len = 0;` reset removed
#                   must fail, or the rerun case shows nothing
#
# usage: scripts/argv-rerun-check.sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/.." && pwd)
cd "$root"
CC_="${CC:-cc}"
[ -x bin/spinel ] || { echo "argv-rerun-check: run make first" >&2; exit 2; }
[ -f lib/libspinel_rt.a ] || { echo "argv-rerun-check: no lib/libspinel_rt.a" >&2; exit 2; }

w=$(mktemp -d "${TMPDIR:-/tmp}/argv-rerun.XXXXXX")
trap 'rm -rf "$w"' EXIT INT TERM
pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok   %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL %s\n' "$1"; [ $# -lt 2 ] || printf '     %s\n' "$2"; }

printf 'ARGV.each { |a| puts a.size }\n' > "$w/av.rb"
cat > "$w/drv.c" <<'C'
int sp_prog_main(int argc, char **argv);
int main(int argc, char **argv) {
  int rc = sp_prog_main(argc, argv);
  if (rc) return rc;
  return sp_prog_main(argc, argv);
}
C
./bin/spinel "$w/av.rb" -c -o "$w/av.c" >/dev/null 2>"$w/gen.err" \
  || { echo "argv-rerun-check: generation failed:"; cat "$w/gen.err"; exit 1; }
grep -q 'sp_argv.len = 0;' "$w/av.c" \
  || { echo "argv-rerun-check: the ARGV fill has no reset"; exit 1; }
sed 's/sp_argv\.len = 0; //' "$w/av.c" > "$w/av_noreset.c"

link() {   # link <c file> <exe>; $CC_ may be a wrapper and a compiler, so it is split
  $CC_ -O1 -Dmain=sp_prog_main -Ilib -c "$1" -o "$1.o" 2>"$w/cc.err" \
    && $CC_ -O1 -c "$w/drv.c" -o "$w/drv.o" 2>>"$w/cc.err" \
    && $CC_ "$1.o" "$w/drv.o" lib/libspinel_rt.a -lm -o "$2" 2>>"$w/cc.err"
}
link "$w/av.c" "$w/av" || { echo "argv-rerun-check: build failed:"; cat "$w/cc.err"; exit 1; }
link "$w/av_noreset.c" "$w/av_noreset" || { echo "argv-rerun-check: control build failed:"; cat "$w/cc.err"; exit 1; }

big=$(head -c 3000 /dev/zero | tr '\0' x)
printf '3000\n1\n2\n3000\n1\n2\n' > "$w/want"
if SPINEL_GC_STRESS=1 MALLOC_PERTURB_=165 "$w/av" "$big" b cc > "$w/out" 2>"$w/err"; then
  cmp -s "$w/out" "$w/want" \
    && ok "a second main in one process sees only its own ARGV slots" \
    || bad "a second main in one process sees only its own ARGV slots" "output: $(tr '\n' ' ' < "$w/out")"
else
  bad "a second main in one process sees only its own ARGV slots" "exit $?"
fi
if SPINEL_GC_STRESS=1 MALLOC_PERTURB_=165 "$w/av_noreset" "$big" b cc > "$w/out2" 2>&1; then
  bad "without the reset the second main fails (control)" "exit 0"
else
  ok "without the reset the second main fails (control)"
fi

printf '%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" = 0 ]
