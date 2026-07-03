#!/bin/sh
# spin end-to-end check: scaffold -> path dep -> git dep -> lock -> vendor ->
# offline -> test, all hermetic (file:// git remote, scratch HOME cache).
# Usage: tools/spin_e2e.sh <spin-binary>   (run from the repo root)
set -e

SPIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
WORK=$(mktemp -d /tmp/spin-e2e.XXXXXX)
export XDG_CACHE_HOME="$WORK/cache"
trap 'rm -rf "$WORK"' EXIT

fail() { echo "spin-e2e FAIL: $1" >&2; exit 1; }
expect() { # expect <label> <want> <got>
  [ "$3" = "$2" ] || fail "$1: want [$2] got [$3]"
}

cd "$WORK"

# --- scaffold + run -----------------------------------------------------------
"$SPIN" new app >/dev/null
cd app
expect "scaffold run" "Hello from app" "$("$SPIN" run 2>&1 | tail -1)"

# --- library gem (path dependency) --------------------------------------------
cd "$WORK"
"$SPIN" new spinel-ansi >/dev/null
rm -rf spinel-ansi/bin
printf '[gem]\nname = "ansi"\n' > spinel-ansi/gem.toml
cat > spinel-ansi/ansi.rb <<'EOF'
module Ansi
  def self.red(s) = "\e[31m" + s + "\e[0m"
end
EOF

# --- git-source gem (file:// remote) -------------------------------------------
mkdir gitgem
cd gitgem
git init -q
printf '[gem]\nname = "greet"\n' > gem.toml
printf 'module Greet\n  def self.hi(n) = "hi " + n\nend\n' > greet.rb
git add gem.toml greet.rb
git -c user.email=spin@e2e -c user.name=spin-e2e commit -qm init
cd "$WORK/app"

printf '[gem]\nname = "app"\n\n[dependencies]\nansi = { path = "../spinel-ansi" }\ngreet = { git = "file://%s/gitgem" }\n' "$WORK" > gem.toml
printf 'require "ansi"\nrequire "greet"\nputs Ansi.red(Greet.hi("spin"))\n' > bin/app.rb

expect "fetch" "fetched 2 gem(s)" "$("$SPIN" fetch 2>&1 | tail -1)"
WANT_OUT=$(printf '\033[31mhi spin\033[0m')
expect "run with deps" "$WANT_OUT" "$("$SPIN" run 2>&1 | tail -1)"

# --- lock ----------------------------------------------------------------------
"$SPIN" lock >/dev/null
[ -f gem.lock ] || fail "gem.lock not written"
grep -q '^\[lock\.greet\]$' gem.lock || fail "gem.lock lacks [lock.greet]"
grep -q '^ref = "[0-9a-f]\{40\}"$' gem.lock || fail "gem.lock lacks a full-SHA ref"

# --- add / remove edit the manifest --------------------------------------------
"$SPIN" add extra --path ../spinel-ansi >/dev/null
grep -q '^extra = ' gem.toml || fail "spin add didn't edit gem.toml"
"$SPIN" remove extra >/dev/null
grep -q '^extra = ' gem.toml && fail "spin remove didn't edit gem.toml"

# --- test: snapshot regen + CRuby parity fallback (both need dep -I) ------------
printf 'require "ansi"\nputs Ansi.red("t")\n' > test/color_test.rb
expect "test (CRuby parity)" "1/1 passed" "$("$SPIN" test 2>&1 | tail -1)"
"$SPIN" test --regen >/dev/null 2>&1
[ -s test/color_test.rb.expected ] || fail "test --regen wrote no snapshot"
expect "test (snapshot)" "1/1 passed" "$("$SPIN" test 2>&1 | tail -1)"

# --- carried native C (M2): gem .c compiled to the shared cache, --link'ed ------
cd "$WORK"
mkdir -p spinel-fast
printf '[gem]\nname = "fast"\n' > spinel-fast/gem.toml
cat > spinel-fast/fast.rb <<'EOF'
module Fast
  ffi_func :fast_quad, [:int], :int
end
EOF
cat > spinel-fast/fast_ext.c <<'EOF'
#include <stdint.h>
intptr_t fast_quad(intptr_t x) { return x * 4; }
EOF
cd "$WORK/app"
printf '[gem]\nname = "app"\n\n[dependencies]\nansi = { path = "../spinel-ansi" }\ngreet = { git = "file://%s/gitgem" }\nfast = { path = "../spinel-fast" }\n' "$WORK" > gem.toml
printf 'require "ansi"\nrequire "greet"\nrequire "fast"\nputs Ansi.red(Greet.hi("spin"))\nputs Fast.fast_quad(10)\n' > bin/app.rb
OUT=$("$SPIN" run 2>&1)
echo "$OUT" | grep -q "^cc fast/fast_ext.c$" || fail "native compile line missing"
expect "carried-C result" "40" "$(echo "$OUT" | tail -1)"
# second build: object cached, no recompile
"$SPIN" clean >/dev/null
OUT=$("$SPIN" run 2>&1)
echo "$OUT" | grep -q "^cc " && fail "native object not reused from cache"
expect "carried-C cached result" "40" "$(echo "$OUT" | tail -1)"
# touching the .c invalidates the cached object and the app binary
sleep 1
touch ../spinel-fast/fast_ext.c
OUT=$("$SPIN" run 2>&1)
echo "$OUT" | grep -q "^cc fast/fast_ext.c$" || fail "touched .c not recompiled"

# --- unresolved require is a hard error (spin sets SPINEL_REQUIRE_GATE) ---------
printf 'require "nosuchgem"\nputs 1\n' > bin/broken.rb
if "$SPIN" build broken >/dev/null 2>"$WORK/gate.err"; then
  fail "unresolved require compiled anyway"
fi
grep -q "nosuchgem" "$WORK/gate.err" || fail "gate error doesn't name the missing gem"
rm -f bin/broken.rb

# --- vendor -> offline, with and without the lock -------------------------------
"$SPIN" vendor >/dev/null
rm -rf "$XDG_CACHE_HOME"
"$SPIN" clean >/dev/null
OUT=$(SPIN_OFFLINE=1 "$SPIN" run 2>&1)
expect "offline (locked, vendored)" "$WANT_OUT
40" "$(echo "$OUT" | tail -2)"
rm -f gem.lock
"$SPIN" clean >/dev/null
OUT=$(SPIN_OFFLINE=1 "$SPIN" run 2>&1)
expect "offline (no lock)" "$WANT_OUT
40" "$(echo "$OUT" | tail -2)"

# --- list / tree -----------------------------------------------------------------
"$SPIN" list | grep -q "^ansi 0.0.0 (path " || fail "spin list lacks the path dep"
"$SPIN" list | grep -q "^greet 0.0.0 (git " || fail "spin list lacks the git dep"
"$SPIN" list --json | grep -q '^\[{"name":"' || fail "spin list --json shape"
"$SPIN" tree | grep -q "^  fast 0.0.0" || fail "spin tree lacks the nested dep line"
"$SPIN" tree --json | grep -q '"deps":\[' || fail "spin tree --json shape"

# --- index (M3): TOML index, MVS selection, search ------------------------------
cd "$WORK"
mkdir hello
cd hello
git init -q
printf '[gem]\nname = "hello"\nversion = "1.0.0"\n' > gem.toml
printf 'module Hello\n  def self.greet = "hello v1"\nend\n' > hello.rb
git add gem.toml hello.rb
git -c user.email=spin@e2e -c user.name=spin-e2e commit -qm v1
SHA1=$(git rev-parse HEAD)
printf '[gem]\nname = "hello"\nversion = "1.1.0"\n' > gem.toml
printf 'module Hello\n  def self.greet = "hello v11"\nend\n' > hello.rb
git add gem.toml hello.rb
git -c user.email=spin@e2e -c user.name=spin-e2e commit -qm v11
SHA2=$(git rev-parse HEAD)
cd "$WORK"
mkdir -p index/gems
cd index
git init -q
printf 'name = "hello"\nrepo = "file://%s/hello"\n\n[[release]]\nversion = "1.0.0"\nref = "%s"\n\n[[release]]\nversion = "1.1.0"\nref = "%s"\n' "$WORK" "$SHA1" "$SHA2" > gems/hello.toml
git add gems
git -c user.email=spin@e2e -c user.name=spin-e2e commit -qm seed
cd "$WORK"
export SPIN_INDEX="file://$WORK/index"
"$SPIN" new idxapp >/dev/null
cd idxapp
printf '[gem]\nname = "idxapp"\n\n[dependencies]\nhello = ">= 1.0"\n' > gem.toml
printf 'require "hello"\nputs Hello.greet\n' > bin/idxapp.rb
expect "index MVS (lowest satisfying)" "hello v1" "$("$SPIN" run 2>&1 | tail -1)"
"$SPIN" lock >/dev/null
grep -q '^version = "1.0.0"$' gem.lock || fail "index lock lacks the selected version"
grep -q "^ref = \"$SHA1\"$" gem.lock || fail "index lock lacks the release SHA"
printf '[gem]\nname = "idxapp"\n\n[dependencies]\nhello = "~> 1.1"\n' > gem.toml
OUT=$("$SPIN" run 2>&1)
echo "$OUT" | grep -q "reselecting 1.1.0" || fail "constraint change didn't reselect"
expect "index reselect" "hello v11" "$(echo "$OUT" | tail -1)"
"$SPIN" search hell | grep -q "^hello 1.1.0 " || fail "spin search misses the gem"
printf '[gem]\nname = "idxapp"\n\n[dependencies]\n' > gem.toml
"$SPIN" add hello --version "~> 1.0" >/dev/null
grep -q '^hello = "~> 1.0"$' gem.toml || fail "spin add --version didn't write the constraint"
"$SPIN" lock >/dev/null
"$SPIN" vendor >/dev/null
rm -rf "$XDG_CACHE_HOME/spinel/gems" "$XDG_CACHE_HOME/spinel/index"
"$SPIN" clean >/dev/null
expect "index offline (locked, vendored)" "hello v1" "$(SPIN_OFFLINE=1 "$SPIN" run 2>&1 | tail -1)"
unset SPIN_INDEX

echo "spin-e2e: ALL GREEN"
