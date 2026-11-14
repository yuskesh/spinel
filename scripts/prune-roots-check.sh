#!/bin/sh
# The prune of unnamed methods (prune_unnamed_methods, src/analyze_desugar.c)
# keeps the methods named from outside the program text: an --analysis-root
# method and a --core identity that no call in the program names.
#
# The --emit-mlir output holds the table before and after analysis; a def
# the prune blanked is a NilNode in the second. What each case would catch:
#   analysis root, uncalled   the prune blanked the root's def, and the
#                             request was refused as an unknown method
#   others still blanked      the keep-list kept more than the roots: a def
#                             no root names must still be blanked
#   extern, uncalled          a $SPINEL_EXTERN_METHODS entry was blanked
#   unknown analysis root     the keep-list does not make every root request
#                             succeed: a method the program lacks is still
#                             refused
#   core, uncalled            the prune blanked the selected def, and --core
#                             failed with "matches no method" (--core does not
#                             combine with --emit-mlir, so the mapping is read)
#
# usage: scripts/prune-roots-check.sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/.." && pwd)
cd "$root"
[ -x bin/spinel ] || { echo "prune-roots-check: run make first" >&2; exit 2; }
unset SPINEL_KEEP_UNNAMED SPINEL_EXTERN_METHODS 2>/dev/null || true

w=$(mktemp -d "${TMPDIR:-/tmp}/prune-roots.XXXXXX")
trap 'rm -rf "$w"' EXIT INT TERM
pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok   %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL %s\n' "$1"; [ $# -lt 2 ] || printf '     %s\n' "$2"; }

cat > "$w/p.rb" <<'RB'
def sq(x)
  x * x
end
def co(b)
  b * 2
end
def zz(y)
  y + 1
end
def ex(a)
  a
end
puts 1
RB

# the kind of the def named $2 in the analysed table of $1 (its last record)
kind() { grep -o "kind = \"[A-Za-z]*\", strings = {\"name\" = \"$2\"" "$1" | tail -1 | cut -d'"' -f2; }

if ./bin/spinel --emit-mlir="$w/root.mlir" --analysis-root=sq:int "$w/p.rb" >/dev/null 2>"$w/root.err"; then
  grep -q 'method = "sq"' "$w/root.mlir" && [ "$(kind "$w/root.mlir" sq)" = DefNode ] \
    && ok "an uncalled --analysis-root method is analysed" \
    || bad "an uncalled --analysis-root method is analysed" "sq: $(kind "$w/root.mlir" sq)"
  [ "$(kind "$w/root.mlir" zz)" = NilNode ] && [ "$(kind "$w/root.mlir" ex)" = NilNode ] \
    && [ "$(kind "$w/root.mlir" co)" = NilNode ] \
    && ok "the defs no root names are still blanked" \
    || bad "the defs no root names are still blanked" \
         "zz: $(kind "$w/root.mlir" zz) ex: $(kind "$w/root.mlir" ex) co: $(kind "$w/root.mlir" co)"
else
  bad "an uncalled --analysis-root method is analysed" "$(head -1 "$w/root.err")"
fi

if SPINEL_EXTERN_METHODS=ex ./bin/spinel --emit-mlir="$w/ext.mlir" --analysis-root=sq:int "$w/p.rb" >/dev/null 2>"$w/ext.err"; then
  [ "$(kind "$w/ext.mlir" ex)" = DefNode ] && [ "$(kind "$w/ext.mlir" zz)" = NilNode ] \
    && ok "an uncalled \$SPINEL_EXTERN_METHODS method is kept" \
    || bad "an uncalled \$SPINEL_EXTERN_METHODS method is kept" \
         "ex: $(kind "$w/ext.mlir" ex) zz: $(kind "$w/ext.mlir" zz)"
else
  bad "an uncalled \$SPINEL_EXTERN_METHODS method is kept" "$(head -1 "$w/ext.err")"
fi

if ./bin/spinel --emit-mlir="$w/none.mlir" --analysis-root=nosuch:int "$w/p.rb" >/dev/null 2>"$w/none.err"; then
  bad "an --analysis-root the program lacks is refused" "exit 0"
else
  grep -q 'unknown top-level method' "$w/none.err" \
    && ok "an --analysis-root the program lacks is refused" \
    || bad "an --analysis-root the program lacks is refused" "$(head -1 "$w/none.err")"
fi

if ./bin/spinel "$w/p.rb" --core co --core-id=deadbeef --core-map="$w/map.tsv" \
     --no-line-map -S > "$w/core.c" 2>"$w/core.err"; then
  awk -F'\t' 'NR==2 && $1=="co" && $2=="spc_deadbeef_co" {f=1} END {exit !f}' "$w/map.tsv" \
    && ok "an uncalled --core method is mapped" \
    || bad "an uncalled --core method is mapped" "$(sed -n 2p "$w/map.tsv")"
else
  bad "an uncalled --core method is mapped" "$(head -1 "$w/core.err")"
fi

printf '%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" = 0 ]
