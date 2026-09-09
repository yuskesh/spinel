#!/bin/sh
# Run the test corpus through the product --arena path and classify what happens.
#
# This does not compile the tests itself. It calls bin/spinel with --arena, so
# every case goes through the same driver the product uses: the same define on
# the generated translation unit, the same runtime archive, and the same
# refusals. A test-only compile line could drift from the product; this cannot.
#
# Classification is by what the driver did, never by the test's name:
#
#   pass       built, ran, output matched the expectation
#   fail       built and ran, output did not match
#   rejected   the driver refused the build and said why. Each of these is a
#              negative control: the refusal has to be non-zero and carry a
#              reason, and these are counted separately rather than folded into
#              a pass rate
#   error      the driver failed some other way, or the program did not run.
#              None of these is expected; each needs diagnosis
#   noexp      no .expected file, so there is nothing to compare against
#
# usage: scripts/arena-suite.sh [-j N] [-o RESULTS.tsv]
#        scripts/arena-suite.sh --one <test.rb>      (one case, prints its row)
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/.." && pwd)
cd "$root"
CC_="${CC:-cc}"
SPINEL="$root/bin/spinel"
# The suite compiles every case with --int-overflow=promote (Makefile SP_OV_FLAG),
# and several tests only make sense in that mode, so this must match it or the
# comparison is against a different program.
OV_FLAG="--int-overflow=promote"
# ARENA=0 runs the same harness with the collector, as the control that says
# whether a failure belongs to the configuration or to this harness.
ARENA="${ARENA:-1}"

one() {
  t=$1
  name=$(printf '%s' "$t" | sed -e 's|^test/||' -e 's|^packages/||' -e 's|/test/|.|' -e 's|\.rb$||')
  d=$(mktemp -d "${TMPDIR:-/tmp}/arena-one.XXXXXX")
  if [ "$ARENA" = "1" ]; then mode=--arena; else mode=; fi
  # shellcheck disable=SC2086
  if ! "$SPINEL" "$t" $mode $OV_FLAG --cc="$CC_" -o "$d/bin" >"$d/build.out" 2>&1; then
    if grep -q -- '--arena refuses\|--arena is single-threaded only' "$d/build.out"; then
      why=$(grep -o -- '--arena refuses [^:]*\|--arena is single-threaded only' "$d/build.out" | head -1)
      printf 'rejected\t%s\t%s\n' "$name" "$why"
    else
      printf 'error\t%s\t%s\n' "$name" "$(head -1 "$d/build.out" | tr '\t' ' ' | cut -c1-160)"
    fi
    rm -rf "$d"; return 0
  fi
  exp="$t.expected"
  if [ ! -f "$exp" ]; then printf 'noexp\t%s\t-\n' "$name"; rm -rf "$d"; return 0; fi
  args=""; [ -f "$t.args" ] && args=$(cat "$t.args")
  stdinf=/dev/null; [ -f "$t.stdin" ] && stdinf="$t.stdin"
  # shellcheck disable=SC2086
  if timeout 10 "$d/bin" $args <"$stdinf" >"$d/out" 2>"$d/err"; then rc=0; else rc=$?; fi
  LC_ALL=C sed 's/\r$//' "$exp" > "$d/exp"
  LC_ALL=C sed 's/\r$//' "$d/out" > "$d/out.n"
  if cmp -s "$d/out.n" "$d/exp"; then
    printf 'pass\t%s\t-\n' "$name"
  else
    printf 'fail\t%s\trc=%s %s\n' "$name" "$rc" \
      "$(diff "$d/exp" "$d/out.n" 2>/dev/null | head -2 | tr '\n\t' '  ' | cut -c1-160)"
  fi
  rm -rf "$d"; return 0
}

if [ "${1:-}" = "--one" ]; then
  [ $# -eq 2 ] || { echo "usage: $0 --one <test.rb>" >&2; exit 2; }
  one "$2"; exit 0
fi

jobs=8
out="$root/build/arena-results.tsv"
while [ $# -gt 0 ]; do
  case "$1" in
    -j) jobs=$2; shift 2 ;;
    -j*) jobs=${1#-j}; shift ;;
    -o) out=$2; shift 2 ;;
    -o*) out=${1#-o}; shift ;;
    *)  echo "usage: $0 [-j N] [-o RESULTS.tsv]" >&2; exit 2 ;;
  esac
done

[ -x "$SPINEL" ] || { echo "arena-suite: no bin/spinel; run make first" >&2; exit 2; }
[ -f lib/libspinel_rt_arena.a ] || { echo "arena-suite: no lib/libspinel_rt_arena.a; run make arena-archive" >&2; exit 2; }

work=$(mktemp -d "${TMPDIR:-/tmp}/arena-suite.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM
mkdir -p "$(dirname "$out")"

# The corpus is the Makefile's own TESTS and PKG_TESTS, read from it rather
# than reconstructed. Reimplementing its filters put six cases on the wrong
# side: it excludes four promote_* tests this build does not run, and includes
# the two regexp_unicode ones and the eight openssl ones that a hand-written
# filter dropped.
{ make -n -p 2>/dev/null | grep -m1 '^TESTS :*=' | cut -d= -f2-
  make -n -p 2>/dev/null | grep -m1 '^PKG_TESTS :*=' | cut -d= -f2-
} | tr ' ' '\n' | grep '\.rb$' | sort -u > "$work/corpus"
total=$(wc -l < "$work/corpus" | tr -d ' ')
echo "arena-suite: $total cases through bin/spinel --arena, -j$jobs" >&2

: > "$out"
CC="$CC_" ARENA="$ARENA" xargs -a "$work/corpus" -P "$jobs" -I{} "$here/arena-suite.sh" --one {} >> "$out" 2>/dev/null

printf '\n'
for v in pass rejected fail error noexp; do
  printf '%-9s %s\n' "$v" "$(grep -c "^$v	" "$out" || true)"
done
printf '%-9s %s\n' "rows" "$(wc -l < "$out" | tr -d ' ')"
printf '%-9s %s\n' "corpus" "$total"
echo "results: $out"
[ "$(grep -c '^fail	\|^error	' "$out" || true)" = "0" ]
