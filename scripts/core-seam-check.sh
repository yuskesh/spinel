#!/bin/sh
# The --core seam: a selected root gets a real symbol from the mapping, its body
# is not emitted, and the call site names the same symbol.
#
# The point of each case is what it would catch:
#   correct core     the seam works at all
#   wrong core       the call REACHES the core object -- a hosted body left
#                    behind would print 3 here and look like success
#   missing core     a missing body is a link failure, not a silent fallback
#   wrong snapshot   a declaration and a definition from different builds do
#                    not link, which is the backstop behind the driver's own
#                    snapshot check rather than a substitute for it
#
# usage: scripts/core-seam-check.sh [--cc CMD]
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/.." && pwd)
cd "$root"
CC_="${CC:-cc}"
while [ $# -gt 0 ]; do
  case "$1" in
    --cc) CC_=$2; shift 2 ;; --cc=*) CC_=${1#--cc=}; shift ;;
    *) echo "usage: $0 [--cc CMD]" >&2; exit 2 ;;
  esac
done
[ -x bin/spinel ] || { echo "core-seam-check: run make first" >&2; exit 2; }
[ -f lib/libspinel_rt.a ] || { echo "core-seam-check: no lib/libspinel_rt.a" >&2; exit 2; }

w=$(mktemp -d "${TMPDIR:-/tmp}/core-seam.XXXXXX")
trap 'rm -rf "$w"' EXIT INT TERM
pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok   %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL %s\n' "$1"; [ $# -lt 2 ] || printf '     %s\n' "$2"; }

cat > "$w/lower.rb" <<'RB'
def lower(a, b)
  a < b ? a : b
end

puts lower(7, 3)
RB

ID=deadbeef
./bin/spinel "$w/lower.rb" --core lower --core-id=$ID --core-map="$w/map.tsv" \
  --no-line-map -S > "$w/core.c" 2>"$w/gen.err" \
  || { echo "core-seam-check: generation failed:"; cat "$w/gen.err"; exit 1; }

sym=$(awk -F'\t' 'NR==2{print $2}' "$w/map.tsv")
[ -n "$sym" ] && ok "the mapping names a symbol ($sym)" || bad "the mapping names a symbol"
grep -q "sp_int $sym(sp_int lv_a, sp_int lv_b);" "$w/core.c" \
  && ok "the selected root is declared with the inferred signature" \
  || bad "the selected root is declared with the inferred signature"
[ "$(grep -c "sp_int $sym(sp_int lv_a, sp_int lv_b) {" "$w/core.c")" = "0" ] \
  && ok "no body is emitted for the selected root" \
  || bad "no body is emitted for the selected root"
grep -q "$sym(7LL, 3LL)" "$w/core.c" \
  && ok "the call site names the mapped symbol" \
  || bad "the call site names the mapped symbol" "$(grep -m1 '7LL, 3LL' "$w/core.c")"
[ "$(grep -c 'sp_lower' "$w/core.c")" = "0" ] \
  && ok "the old static name is gone from the C entirely" \
  || bad "the old static name is gone from the C entirely"

printf 'typedef long I;\nI %s(I a, I b){ return a < b ? a : b; }\n' "$sym"    > "$w/ci.c"
printf 'typedef long I;\nI %s(I a, I b){ return 999; }\n'          "$sym"    > "$w/cw.c"
: > "$w/cm.c"
printf 'typedef long I;\nI spc_cafe1234_lower(I a, I b){ return a < b ? a : b; }\n' > "$w/co.c"
for f in ci cw cm co; do $CC_ -O2 -c "$w/$f.c" -o "$w/$f.o"; done
LIBS="-lm"; printf 'int main(void){return 0;}\n' > "$w/p.c"
$CC_ "$w/p.c" -lcrypt -o "$w/p.bin" 2>/dev/null && LIBS="-lm -lcrypt"
link() { $CC_ -O2 -DSP_INT_OVERFLOW_MODE_PROMOTE -Ilib "$w/core.c" "$1" \
           lib/libspinel_rt.a $LIBS -o "$2" 2>"$3"; }

if link "$w/ci.o" "$w/ok.bin" "$w/e1"; then
  [ "$("$w/ok.bin")" = "3" ] && ok "a correct core object prints 3" \
    || bad "a correct core object prints 3" "got [$("$w/ok.bin")]"
else bad "a correct core object links" "$(head -2 "$w/e1")"; fi

if link "$w/cw.o" "$w/bad.bin" "$w/e2"; then
  [ "$("$w/bad.bin")" = "999" ] \
    && ok "a deliberately wrong core object is what runs (999), so the call routes there" \
    || bad "the call routes to the core object" "got [$("$w/bad.bin")], a hosted body may remain"
else bad "the wrong-core control links"; fi

link "$w/cm.o" "$w/no.bin" "$w/e3" \
  && bad "a missing core object fails the link" "it linked, so something else provided the body" \
  || { grep -q "undefined reference to .$sym.\|Undefined symbols.*$sym" "$w/e3" \
       && ok "a missing core object fails the link, naming the symbol" \
       || bad "a missing core object fails the link, naming the symbol" "$(head -2 "$w/e3")"; }

link "$w/co.o" "$w/mis.bin" "$w/e4" \
  && bad "a different snapshot id fails the link" "it linked" \
  || ok "a core object from a different snapshot id does not link"

printf '\ncore seam: %s passed, %s failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
