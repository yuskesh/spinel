#!/bin/sh
# Run the test corpus through the product --arena path and classify what happens.
#
# This does not compile the tests itself. It calls bin/spinel with --arena, so
# every case goes through the same driver the product uses: the same define on
# the generated translation unit, the same runtime archive, and the same
# refusals. A test-only compile line could drift from the product; this cannot.
#
# The comparison is the one `make test` uses (RUN_ONE_TEST): stdout against
# <test>.expected AND stderr against <test>.err.expected, where a missing
# .err.expected means "stderr must be empty". Comparing stdout alone let a run
# that printed the right answer and then died pass.
#
# Classification is by what the driver and the program did, never by the test's
# name:
#
#   pass       built, ran, stdout AND stderr matched
#   fail       built and ran, output did not match
#   capacity   ran and hit the arena limit: the exhaustion diagnostic on stderr
#              and a non-zero exit. This is the configuration's own contract
#              coming due, not a wrong answer, so it is its own verdict -- it is
#              never folded into pass, and never reported as a plain failure
#   timeout    the run hit the time limit. Always a failure of the gate
#   rejected   the driver refused the build and said why. Each of these is a
#              negative control: the refusal has to be non-zero and carry a
#              reason
#   error      the driver failed some other way, or the program did not run.
#              None of these is expected; each needs diagnosis
#   noexp      no .expected file, so there is nothing to compare against
#
# THE GATE, and what it is measured against. "No failures" is NOT the standard,
# because it is not the standard the collector meets either. This runs the
# PRODUCT path (bin/spinel), whose compile line is not the one `make test` uses
# (RUN_ONE_TEST compiles the generated C itself, with a precompiled header and
# its own flags), and a number of cases already fail or fail to build there with
# the collector, before --arena is mentioned. int_arg_into_bigint_param is one:
# PASS under `make test`, SIGSEGV through bin/spinel in BOTH configurations.
# Gating on an absolute zero would mostly measure that gap.
#
# The standard is therefore the SAME corpus through the SAME driver with the
# collector -- the ARENA=0 leg -- and the gate is that nothing gets worse:
#
#   ARENA=0 scripts/arena-suite.sh -o build/arena-baseline.tsv
#           scripts/arena-suite.sh -o build/arena-results.tsv --against build/arena-baseline.tsv
#
# Every case whose (baseline, arena) verdict pair is not (pass, pass) is
# declared in scripts/arena-expected.tsv, and the two sets are compared IN BOTH
# DIRECTIONS. A new failure, a new refusal, a new capacity case, a changed kind,
# and a case that quietly stopped being refused are all equally loud. Nothing is
# treated as success for being an expected KIND of error: the declaration names
# the case, not the category. Without --against, a leg is recorded, not gated,
# and says so.
#
# --from RESULTS.tsv applies the gate to a leg that has already been recorded,
# without running it again. It is how the gate's own negative controls are run
# (does a missing declaration line fail? an extra one? a dropped row?) without
# four minutes of compiling per control, and how a recorded leg is re-checked
# against a revised declaration.
#
# usage: scripts/arena-suite.sh [-j N] [-o RESULTS.tsv] [--against BASE.tsv] [--declare]
#        scripts/arena-suite.sh --from RESULTS.tsv --against BASE.tsv
#        scripts/arena-suite.sh --one <test.rb>      (one case, prints its row)
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/.." && pwd)
cd "$root"
# CC is a COMMAND, not a program: the Makefile's auto-ccache path makes it
# "/usr/bin/ccache cc". It is passed on as one --cc= word for that reason.
CC_="${CC:-cc}"
SPINEL="$root/bin/spinel"
# The suite compiles every case with --int-overflow=promote (Makefile SP_OV_FLAG),
# and several tests only make sense in that mode, so this must match it or the
# comparison is against a different program.
OV_FLAG="--int-overflow=promote"
# ARENA=0 runs the same harness with the collector, as the control that says
# whether a failure belongs to the configuration or to this harness.
ARENA="${ARENA:-1}"
DECLARED="$root/scripts/arena-expected.tsv"

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
  LC_ALL=C sed 's/\r$//' "$d/err" > "$d/err.n"
  # Same rule as RUN_ONE_TEST: a missing .err.expected means stderr must be empty.
  if [ -f "$t.err.expected" ]; then LC_ALL=C sed 's/\r$//' "$t.err.expected" > "$d/experr.n"
  else : > "$d/experr.n"; fi
  # The arena's own contract coming due, recognised by the diagnostic the
  # runtime prints, not by a bare exit code -- so an unrelated exit(1) does not
  # get filed under capacity. All THREE of the runtime's capacity diagnostics
  # are listed: the limit ("arena exhausted"), the degenerate-size refusals
  # ("arena request refused: ..."), and the chunk calloc failure. Matching only
  # the newest of them filed five real capacity cases as plain failures.
  cap='^spinel: arena exhausted$|^spinel: arena request refused: |^spinel: arena: calloc\('
  if grep -qE "$cap" "$d/err.n"; then
    printf 'capacity\t%s\trc=%s %s\n' "$name" "$rc" \
      "$(grep -m1 -E "$cap" "$d/err.n" | tr '\t' ' ' | cut -c1-120)"
  elif [ "$rc" = "124" ]; then
    printf 'timeout\t%s\trc=124 killed at 10s\n' "$name"
  elif cmp -s "$d/out.n" "$d/exp" && cmp -s "$d/err.n" "$d/experr.n"; then
    printf 'pass\t%s\trc=%s\n' "$name" "$rc"
  else
    printf 'fail\t%s\trc=%s %s\n' "$name" "$rc" \
      "$( { diff "$d/exp" "$d/out.n"; diff "$d/experr.n" "$d/err.n"; } 2>/dev/null | head -2 | tr '\n\t' '  ' | cut -c1-160)"
  fi
  rm -rf "$d"; return 0
}

if [ "${1:-}" = "--one" ]; then
  [ $# -eq 2 ] || { echo "usage: $0 --one <test.rb>" >&2; exit 2; }
  one "$2"; exit 0
fi

jobs=8
declare_mode=0
against=""
from=""
out="$root/build/arena-results.tsv"
while [ $# -gt 0 ]; do
  case "$1" in
    -j) jobs=$2; shift 2 ;;
    -j*) jobs=${1#-j}; shift ;;
    -o) out=$2; shift 2 ;;
    -o*) out=${1#-o}; shift ;;
    --against) against=$2; shift 2 ;;
    --from) from=$2; out=$2; shift 2 ;;
    --declare) declare_mode=1; shift ;;
    *)  echo "usage: $0 [-j N] [-o RESULTS.tsv] [--from R.tsv] [--against BASE.tsv] [--declare]" >&2; exit 2 ;;
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
[ "$total" -gt 0 ] || { echo "arena-suite: read an EMPTY corpus from the Makefile" >&2; exit 2; }
echo "arena-suite: $total cases through bin/spinel --arena, -j$jobs" >&2

: > "$work/worker.err"
if [ -n "$from" ]; then
  [ -f "$from" ] || { echo "arena-suite: no such results file: $from" >&2; exit 2; }
  echo "arena-suite: gating the recorded leg $from; nothing is compiled or run" >&2
else
  : > "$out"
  # stderr is kept: a worker that dies has to be visible, not swallowed.
  CC="$CC_" ARENA="$ARENA" xargs -a "$work/corpus" -P "$jobs" -I{} \
    "$here/arena-suite.sh" --one {} >> "$out" 2>"$work/worker.err" || true
fi

printf '\n'
for v in pass rejected capacity fail timeout error noexp; do
  printf '%-9s %s\n' "$v" "$(grep -c "^$v	" "$out" || true)"
done
rows=$(wc -l < "$out" | tr -d ' ')
printf '%-9s %s\n' "rows" "$rows"
printf '%-9s %s\n' "corpus" "$total"
echo "results: $out"

# --- the gate -------------------------------------------------------------
ok=1

# Structural checks first. These hold for either leg, and a failure here means
# the counts above are not describing the whole corpus.
if [ -s "$work/worker.err" ]; then
  echo "arena-suite: FAIL: a worker wrote to stderr:" >&2
  head -5 "$work/worker.err" >&2; ok=0
fi
if [ "$rows" != "$total" ]; then
  echo "arena-suite: FAIL: $rows rows for $total cases -- a case produced no verdict" >&2; ok=0
fi
names=$(cut -f2 "$out" | LC_ALL=C sort -u | wc -l | tr -d ' ')
if [ "$names" != "$total" ]; then
  echo "arena-suite: FAIL: the rows name $names distinct cases, not $total" >&2; ok=0
fi

if [ -z "$against" ]; then
  echo "arena-suite: recorded, NOT gated: no --against BASELINE.tsv." >&2
  echo "arena-suite: the gate is this leg compared with the ARENA=0 leg; see the header." >&2
  [ "$ok" = "1" ]; exit
fi
[ -f "$against" ] || { echo "arena-suite: no baseline $against; run the ARENA=0 leg first" >&2; exit 2; }

awk -F'\t' '{print $2"\t"$1}' "$against" | LC_ALL=C sort > "$work/b.nv"
awk -F'\t' '{print $2"\t"$1}' "$out"     | LC_ALL=C sort > "$work/a.nv"
cut -f1 "$work/b.nv" > "$work/b.n"; cut -f1 "$work/a.nv" > "$work/a.n"
if ! cmp -s "$work/b.n" "$work/a.n"; then
  echo "arena-suite: FAIL: the two legs do not cover the same cases:" >&2
  diff "$work/b.n" "$work/a.n" | head -10 >&2; ok=0
fi

# baseline_verdict <TAB> arena_verdict <TAB> name, for every pair that is not
# (pass, pass). This is the whole of what the declaration governs.
join -t"$(printf '\t')" "$work/b.nv" "$work/a.nv" \
  | awk -F'\t' '!($2=="pass" && $3=="pass"){print $2"\t"$3"\t"$1}' \
  | LC_ALL=C sort > "$work/seen"

printf '\n%s\n' "transitions (baseline -> arena), pass -> pass omitted:"
cut -f1,2 "$work/seen" | LC_ALL=C sort | uniq -c | sort -rn | sed 's/^/  /'

if [ "$declare_mode" = "1" ]; then
  cp "$work/seen" "$DECLARED"
  echo "arena-suite: wrote $(wc -l < "$DECLARED" | tr -d ' ') declared transitions to $DECLARED" >&2
elif [ ! -f "$DECLARED" ]; then
  echo "arena-suite: FAIL: no $DECLARED; run with --declare and review the diff" >&2; ok=0
else
  LC_ALL=C sort "$DECLARED" > "$work/want"
  if cmp -s "$work/want" "$work/seen"; then
    echo "arena-suite: the $(wc -l < "$work/want" | tr -d ' ') declared transitions are exactly the ones seen" >&2
  else
    echo "arena-suite: FAIL: the transition set changed (-declared +seen):" >&2
    diff -u "$work/want" "$work/seen" | tail -n +4 | head -60 >&2; ok=0
  fi
fi

[ "$ok" = "1" ]
