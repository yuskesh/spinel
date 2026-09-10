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


# ---- fail-closed on the selection input -----------------------------------
# Each of these must REFUSE. The first pair is the one that matters most: the
# validation must not depend on --core-map, or omitting the map turns an unknown
# identity into a silent fallback to the ordinary hosted body.
refuses() { # label, args...
  _l=$1; shift
  if ./bin/spinel "$@" >/dev/null 2>"$w/r.err"; then
    bad "$_l" "accepted"
  else
    [ -s "$w/r.err" ] && ok "$_l  ($(head -1 "$w/r.err" | cut -c1-72))" \
                      || bad "$_l" "refused without saying why"
  fi
}
refuses "an unknown identity is refused WITHOUT --core-map" \
  "$w/lower.rb" --core nosuch --core-id=$ID -S
refuses "an unknown identity is refused with --core-map" \
  "$w/lower.rb" --core nosuch --core-id=$ID --core-map="$w/u.tsv" -S
refuses "the same identity twice is refused" \
  "$w/lower.rb" --core lower --core lower --core-id=$ID -S
refuses "an empty snapshot id is refused" \
  "$w/lower.rb" --core lower --core-id= -S
refuses "a snapshot id that is not a C identifier is refused" \
  "$w/lower.rb" --core lower --core-id=a-b -S
refuses "an over-long snapshot id is refused" \
  "$w/lower.rb" --core lower --core-id=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa -S
refuses "a Class.method identity is refused in this slice" \
  "$w/lower.rb" --core M.lower --core-id=$ID -S
refuses "a Class#method identity is refused in this slice" \
  "$w/lower.rb" --core 'K#lower' --core-id=$ID -S

# Two identities that mangle to one symbol. `?` becomes `_p`, so `a?` and `a_p`
# collide; injectivity is checked over symbols, not identities, for this reason.
cat > "$w/coll.rb" <<'RB'
def a?(x)
  x < 1
end

def a_p(x)
  x < 2
end

puts a?(3)
puts a_p(4)
RB
refuses "two identities mapping to one symbol are refused" \
  "$w/coll.rb" --core 'a?' --core a_p --core-id=$ID -S

# An unusable mapping path must fail the build, not be ignored.
refuses "an unwritable mapping path fails the build" \
  "$w/lower.rb" --core lower --core-id=$ID --core-map=/nonexistent-dir/m.tsv -S

# A refused build must not have replaced an existing mapping.
printf 'PREVIOUS\n' > "$w/keep.tsv"
./bin/spinel "$w/lower.rb" --core nosuch --core-id=$ID --core-map="$w/keep.tsv" -S >/dev/null 2>&1 || true
[ "$(cat "$w/keep.tsv")" = "PREVIOUS" ] \
  && ok "a refused build leaves an existing mapping untouched" \
  || bad "a refused build leaves an existing mapping untouched" "it was overwritten"
[ -z "$(ls "$w"/*.tmp 2>/dev/null)" ] \
  && ok "no temporary mapping file is left behind" \
  || bad "no temporary mapping file is left behind"

# A file literally named <map>.tmp is a plausible thing for a caller to keep.
# The mapping writer must not touch it on either path: a fixed temporary name
# would truncate it, and would also be shared by two concurrent invocations.
printf 'SENTINEL\n' > "$w/s.tsv.tmp"
./bin/spinel "$w/lower.rb" --core lower --core-id=$ID --core-map="$w/s.tsv" -S >/dev/null 2>&1
[ "$(cat "$w/s.tsv.tmp")" = "SENTINEL" ] \
  && ok "a file named <map>.tmp is untouched on the success path" \
  || bad "a file named <map>.tmp is untouched on the success path" "it was written to"
printf 'SENTINEL\n' > "$w/s.tsv.tmp"
./bin/spinel "$w/lower.rb" --core nosuch --core-id=$ID --core-map="$w/s.tsv" -S >/dev/null 2>&1 || true
[ "$(cat "$w/s.tsv.tmp")" = "SENTINEL" ] \
  && ok "a file named <map>.tmp is untouched on the failure path" \
  || bad "a file named <map>.tmp is untouched on the failure path" "it was written to"

# Concurrent invocations writing the same destination must not share a
# temporary file. Each should either win or lose cleanly; none may leave a
# half-written mapping or a stray temporary behind.
i=1
while [ $i -le 8 ]; do
  ( ./bin/spinel "$w/lower.rb" --core lower --core-id=id$i \
      --core-map="$w/conc.tsv" -S >/dev/null 2>&1; echo $? > "$w/rc$i" ) &
  i=$((i+1))
done
wait
if grep -qv '^0$' "$w"/rc1 "$w"/rc2 "$w"/rc3 "$w"/rc4 "$w"/rc5 "$w"/rc6 "$w"/rc7 "$w"/rc8 2>/dev/null; then
  bad "eight concurrent writes to one mapping all succeed" "$(cat "$w"/rc* | tr '\n' ' ')"
else
  ok "eight concurrent writes to one mapping all succeed"
fi
[ "$(wc -l < "$w/conc.tsv" | tr -d ' ')" = "2" ] \
  && ok "the mapping left by concurrent writers is one complete row" \
  || bad "the mapping left by concurrent writers is one complete row" "$(wc -l < "$w/conc.tsv") lines"
[ -z "$(ls "$w"/.spinel-core-map.* 2>/dev/null)" ] \
  && ok "no exclusive temporary file is left in the destination directory" \
  || bad "no exclusive temporary file is left in the destination directory"

# A rename that cannot succeed. Nothing removes the destination first, so a
# failure to publish must leave what was already there and fail the build --
# losing a usable mapping to make room for one that could not be published
# would be worse than not publishing. A directory as the destination is a
# rename target that always refuses.
mkdir -p "$w/asdir"; printf 'KEEP\n' > "$w/asdir/marker"
if ./bin/spinel "$w/lower.rb" --core lower --core-id=$ID --core-map="$w/asdir" -S >/dev/null 2>&1; then
  bad "a mapping that cannot be published fails the build" "it succeeded"
else
  [ "$(cat "$w/asdir/marker" 2>/dev/null)" = "KEEP" ] \
    && ok "a mapping that cannot be published fails and leaves the destination alone" \
    || bad "a mapping that cannot be published leaves the destination alone" "it was disturbed"
fi
[ -z "$(ls "$w"/.spinel-core-map.* 2>/dev/null)" ] \
  && ok "a failed publish leaves no temporary file behind" \
  || bad "a failed publish leaves no temporary file behind"

printf '\ncore seam: %s passed, %s failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
