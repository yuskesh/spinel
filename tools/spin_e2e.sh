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

# --- library package (path dependency) --------------------------------------------
cd "$WORK"
"$SPIN" new spinel-ansi >/dev/null
rm -rf spinel-ansi/bin
printf '[package]\nname = "ansi"\n' > spinel-ansi/spin.toml
cat > spinel-ansi/ansi.rb <<'EOF'
module Ansi
  def self.red(s) = "\e[31m" + s + "\e[0m"
end
EOF

# --- git-source package (file:// remote) -------------------------------------------
mkdir gitgem
cd gitgem
git init -q
printf '[package]\nname = "greet"\n' > spin.toml
printf 'module Greet\n  def self.hi(n) = "hi " + n\nend\n' > greet.rb
git add spin.toml greet.rb
git -c user.email=spin@e2e -c user.name=spin-e2e commit -qm init
cd "$WORK/app"

printf '[package]\nname = "app"\n\n[dependencies]\nansi = { path = "../spinel-ansi" }\ngreet = { git = "file://%s/gitgem" }\n' "$WORK" > spin.toml
printf 'require "ansi"\nrequire "greet"\nputs Ansi.red(Greet.hi("spin"))\n' > bin/app.rb

expect "fetch" "fetched 2 package(s)" "$("$SPIN" fetch 2>&1 | tail -1)"
WANT_OUT=$(printf '\033[31mhi spin\033[0m')
expect "run with deps" "$WANT_OUT" "$("$SPIN" run 2>&1 | tail -1)"

# --- lock ----------------------------------------------------------------------
"$SPIN" lock >/dev/null
[ -f spin.lock ] || fail "spin.lock not written"
grep -q '^\[lock\.greet\]$' spin.lock || fail "spin.lock lacks [lock.greet]"
grep -q '^ref = "[0-9a-f]\{40\}"$' spin.lock || fail "spin.lock lacks a full-SHA ref"

# --- add / remove edit the manifest --------------------------------------------
"$SPIN" add extra --path ../spinel-ansi >/dev/null
grep -q '^extra = ' spin.toml || fail "spin add didn't edit spin.toml"
"$SPIN" remove extra >/dev/null
grep -q '^extra = ' spin.toml && fail "spin remove didn't edit spin.toml"

# --- test: snapshot regen + CRuby parity fallback (both need dep -I) ------------
printf 'require "ansi"\nputs Ansi.red("t")\n' > test/color_test.rb
expect "test (CRuby parity)" "1/1 passed" "$("$SPIN" test 2>&1 | tail -1)"
"$SPIN" test --regen >/dev/null 2>&1
[ -s test/color_test.rb.expected ] || fail "test --regen wrote no snapshot"
expect "test (snapshot)" "1/1 passed" "$("$SPIN" test 2>&1 | tail -1)"

# --- carried native C (M2): package .c compiled to the shared cache, --link'ed ------
cd "$WORK"
mkdir -p spinel-fast
printf '[package]\nname = "fast"\n' > spinel-fast/spin.toml
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
printf '[package]\nname = "app"\n\n[dependencies]\nansi = { path = "../spinel-ansi" }\ngreet = { git = "file://%s/gitgem" }\nfast = { path = "../spinel-fast" }\n' "$WORK" > spin.toml
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
rm -f spin.lock
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
printf '[package]\nname = "hello"\nversion = "1.0.0"\n' > spin.toml
printf 'module Hello\n  def self.greet = "hello v1"\nend\n' > hello.rb
git add spin.toml hello.rb
git -c user.email=spin@e2e -c user.name=spin-e2e commit -qm v1
SHA1=$(git rev-parse HEAD)
printf '[package]\nname = "hello"\nversion = "1.1.0"\n' > spin.toml
printf 'module Hello\n  def self.greet = "hello v11"\nend\n' > hello.rb
git add spin.toml hello.rb
git -c user.email=spin@e2e -c user.name=spin-e2e commit -qm v11
SHA2=$(git rev-parse HEAD)
cd "$WORK"
mkdir -p index/packages
cd index
git init -q
printf 'name = "hello"\nrepo = "file://%s/hello"\n\n[[release]]\nversion = "1.0.0"\nref = "%s"\n\n[[release]]\nversion = "1.1.0"\nref = "%s"\n' "$WORK" "$SHA1" "$SHA2" > packages/hello.toml
git add packages
git -c user.email=spin@e2e -c user.name=spin-e2e commit -qm seed
cd "$WORK"
export SPIN_INDEX="file://$WORK/index"
"$SPIN" new idxapp >/dev/null
cd idxapp
printf '[package]\nname = "idxapp"\n\n[dependencies]\nhello = ">= 1.0"\n' > spin.toml
printf 'require "hello"\nputs Hello.greet\n' > bin/idxapp.rb
expect "index MVS (lowest satisfying)" "hello v1" "$("$SPIN" run 2>&1 | tail -1)"
"$SPIN" lock >/dev/null
grep -q '^version = "1.0.0"$' spin.lock || fail "index lock lacks the selected version"
grep -q "^ref = \"$SHA1\"$" spin.lock || fail "index lock lacks the release SHA"
printf '[package]\nname = "idxapp"\n\n[dependencies]\nhello = "~> 1.1"\n' > spin.toml
OUT=$("$SPIN" run 2>&1)
echo "$OUT" | grep -q "reselecting 1.1.0" || fail "constraint change didn't reselect"
expect "index reselect" "hello v11" "$(echo "$OUT" | tail -1)"
"$SPIN" search hell | grep -q "^hello 1.1.0 " || fail "spin search misses the gem"
printf '[package]\nname = "idxapp"\n\n[dependencies]\n' > spin.toml
"$SPIN" add hello --version "~> 1.0" >/dev/null
grep -q '^hello = "~> 1.0"$' spin.toml || fail "spin add --version didn't write the constraint"
"$SPIN" lock >/dev/null
"$SPIN" vendor >/dev/null
rm -rf "$XDG_CACHE_HOME/spin/packages" "$XDG_CACHE_HOME/spin/index"
"$SPIN" clean >/dev/null
expect "index offline (locked, vendored)" "hello v1" "$(SPIN_OFFLINE=1 "$SPIN" run 2>&1 | tail -1)"
unset SPIN_INDEX

# --- publish (M4): validations + --direct into the index -------------------------
export SPIN_INDEX="file://$WORK/index"
git -C "$WORK/index" config receive.denyCurrentBranch updateInstead
cd "$WORK"
"$SPIN" new publib --lib >/dev/null
cd publib
printf '[package]\nname = "publib"\nversion = "0.1.0"\n' > spin.toml
printf 'module Publib\n  def self.hi = "hi"\nend\n' > publib.rb
git init -q
git add spin.toml publib.rb .gitignore
git -c user.email=spin@e2e -c user.name=spin-e2e commit -qm v1
git init -q --bare "$WORK/publib-remote.git"
git remote add origin "$WORK/publib-remote.git"
git push -q origin HEAD
git fetch -q origin
OUT=$("$SPIN" publish --direct --repo https://example.com/you/spinel-publib 2>&1) \
  && fail "publish without tests must be refused"
echo "$OUT" | grep -q "publish requires tests" || fail "missing-tests message"
printf 'require "publib"\nputs Publib.hi\n' > test/hi_test.rb
git add test/hi_test.rb
git -c user.email=spin@e2e -c user.name=spin-e2e commit -qm tests
git push -q origin HEAD
git fetch -q origin
expect "publish --direct" "published publib 0.1.0 (direct)" "$("$SPIN" publish --direct --repo https://example.com/you/spinel-publib 2>&1 | tail -1)"
grep -q '^ref = "[0-9a-f]\{40\}"$' "$WORK/index/packages/publib.toml" || fail "published entry lacks a full SHA"
OUT=$("$SPIN" publish --direct --repo https://example.com/you/spinel-publib 2>&1) \
  && fail "duplicate publish must be refused"
echo "$OUT" | grep -q "already in the index" || fail "duplicate message"
OUT=$("$SPIN" publish --direct --repo https://example.com/other/spinel-publib 2>&1) \
  && fail "same name, different repo must be refused"
echo "$OUT" | grep -q "name policy" || fail "name-policy message"
"$SPIN" search publib | grep -q "^publib 0.1.0 " || fail "published package missing from search"

# --- R8 probes: publish records a pass; resolution warns on recorded fails -------
grep -q '^\[\[probe\]\]$' "$WORK/index/packages/publib.toml" || fail "publish wrote no probe record"
grep -q '^result = "pass"$' "$WORK/index/packages/publib.toml" || fail "probe record isn't a pass"
REV=$(cd "$WORK" && "$(dirname "$SPIN")/spinel" --version | awk '{print $2}')
printf '\n[[probe]]\nversion = "0.1.0"\nspinel = "%s"\nresult = "fail"\ndetail = "e2e-injected"\ndate = "2026-01-01"\n' "$REV" >> "$WORK/index/packages/publib.toml"
git -C "$WORK/index" add packages/publib.toml
git -C "$WORK/index" -c user.email=spin@e2e -c user.name=spin-e2e commit -qm failprobe
cd "$WORK"
"$SPIN" new probeapp >/dev/null
cd probeapp
printf '[package]\nname = "probeapp"\n\n[dependencies]\npublib = "0.1.0"\n' > spin.toml
rm -rf "$XDG_CACHE_HOME/spin/index"
OUT=$("$SPIN" fetch 2>&1 || true)
echo "$OUT" | grep -q "recorded FAILING with this compiler build" || fail "exact-build fail probe didn't warn"
unset SPIN_INDEX

# --- install: build + copy bin/ onto a prefix, uninstall removes ----------------
cd "$WORK/app"
"$SPIN" install --prefix "$WORK/binhome" >/dev/null
OUT=$("$WORK/binhome/app" | tail -2)
expect "installed binary runs" "$WANT_OUT
40" "$OUT"
"$SPIN" install --uninstall --prefix "$WORK/binhome" >/dev/null
[ -e "$WORK/binhome/app" ] && fail "uninstall left the binary"

# --- [[build]]: declared native build steps (#1820) ------------------------------
# A package vendoring a project with its own build system declares the step;
# it runs at dependent-application build time only (never at fetch), needs
# explicit consent, caches by content key, applies declared patches to a
# scratch copy, and feature-gated entries stay off by default. The same
# package works as the build root (app) and as a path dependency (lib).
export XDG_CONFIG_HOME="$WORK/config"
cd "$WORK"
mkdir -p spinel-mathx/vendor/mx spinel-mathx/patches spinel-mathx/bin
cat > spinel-mathx/spin.toml <<'EOF'
[package]
name = "mathx"
version = "0.1.0"

[[build]]
workdir   = "vendor/mx"
patches   = ["patches/*.patch"]
command   = "${CC:-cc} -O2 -c mx.c -o mx.o && ar rcs libmx.a mx.o"
artifacts = ["libmx.a"]

[[build]]
features  = ["cuda"]
workdir   = "vendor/mx"
command   = "${CC:-cc} -O2 -DCUDA -c mx.c -o mxc.o && ar rcs libmx-cuda.a mxc.o"
artifacts = ["libmx-cuda.a"]

[native]
libs = ["${build.out}/libmx.a", "${build.out}/libmx-cuda.a"]
EOF
printf 'int mx_add(int a, int b) { return a + b; }\n' > spinel-mathx/vendor/mx/mx.c
cat > spinel-mathx/patches/01-bias.patch <<'EOF'
--- a/mx.c
+++ b/mx.c
@@ -1 +1 @@
-int mx_add(int a, int b) { return a + b; }
+int mx_add(int a, int b) { return a + b + 1; }
EOF
printf 'module Mathx\n  ffi_func :mx_add, [:int, :int], :int\nend\n' > spinel-mathx/mathx.rb
printf 'require "mathx"\nputs Mathx.mx_add(20, 21)\n' > spinel-mathx/bin/mxdemo.rb

# app-as-root: unconsented build is refused with instructions
cd spinel-mathx
OUT=$("$SPIN" build 2>&1) && fail "unconsented native build must be refused"
echo "$OUT" | grep -q "declares a native build step" || fail "refusal message"
echo "$OUT" | grep -q "allow-native-build" || fail "refusal hint"
# consented: the patch applied to the scratch copy biases the sum by one
OUT=$("$SPIN" run --allow-native-build 2>&1)
expect "native app-as-root run (patched)" "42" "$(echo "$OUT" | tail -1)"
echo "$OUT" | grep -q '^native mathx:' || fail "native build step didn't run"
# the vendored tree is a read-only input: the patch never touches it
grep -q 'a + b + 1' vendor/mx/mx.c && fail "patch leaked into the vendored tree"
# feature-gated entry stays off by default: only the default artifact exists
N_ART=$(find "$XDG_CACHE_HOME/spin/native" -name 'libmx*.a' | wc -l | tr -d ' ')
expect "feature-gated artifact count" "1" "$N_ART"
# second build reuses the content-keyed cache (no native line)
"$SPIN" clean >/dev/null
OUT=$("$SPIN" run --allow-native-build 2>&1)
echo "$OUT" | grep -q '^native mathx:' && fail "cached native build re-ran"
expect "native cached rerun" "42" "$(echo "$OUT" | tail -1)"
# a content change moves the key and rebuilds
printf 'int mx_add(int a, int b) { return a + b; }\nint mx_two(void) { return 2; }\n' > vendor/mx/mx.c
printf -- '--- a/mx.c\n+++ b/mx.c\n@@ -1,2 +1,2 @@\n-int mx_add(int a, int b) { return a + b; }\n+int mx_add(int a, int b) { return a + b + 1; }\n int mx_two(void) { return 2; }\n' > patches/01-bias.patch
OUT=$("$SPIN" run --allow-native-build 2>&1)
echo "$OUT" | grep -q '^native mathx:' || fail "content change didn't rebuild the native step"
expect "native rebuild after change" "42" "$(echo "$OUT" | tail -1)"

# lib case: a consumer app pulls mathx as a path dependency. A fresh cache:
# a cached artifact skips consent by design (the consented command already
# ran on this machine), so refusal is only observable on a cold cache.
export XDG_CACHE_HOME="$WORK/cache-nb"
cd "$WORK"
mkdir -p nbconsumer/bin
printf '[package]\nname = "nbconsumer"\n\n[dependencies]\nmathx = { path = "../spinel-mathx" }\n' > nbconsumer/spin.toml
printf 'require "mathx"\nputs Mathx.mx_add(100, 100)\n' > nbconsumer/bin/app.rb
cd nbconsumer
# fetch never runs a native build (R2: packages compute nothing at fetch)
"$SPIN" fetch >/dev/null 2>&1
# unconsented dependent build is refused; `spin trust` records durable consent
OUT=$("$SPIN" build 2>&1) && fail "unconsented dependent native build must be refused"
"$SPIN" trust mathx | grep -q '^trusted: mathx' || fail "spin trust"
OUT=$("$SPIN" run 2>&1)
expect "native lib-dependency run (patched)" "201" "$(echo "$OUT" | tail -1)"

# consumer-side features: `spin add --features` records the enablement in the
# manifest (cargo-style; the lock stays resolution-only), the gated [[build]]
# entry runs, and its artifact joins the link line.
cd "$WORK"
printf 'int mx_cuda(void) { return 999; }\n' > spinel-mathx/vendor/mx/cuda.c
cat >> spinel-mathx/spin.toml <<'EOF'

[[build]]
features  = ["cuda"]
workdir   = "vendor/mx"
command   = "${CC:-cc} -O2 -c cuda.c -o cuda.o && ar rcs libmx-cu2.a cuda.o"
artifacts = ["libmx-cu2.a"]

[native]
libs = ["${build.out}/libmx.a", "${build.out}/libmx-cuda.a", "${build.out}/libmx-cu2.a"]
EOF
printf 'module Mathx\n  ffi_func :mx_add, [:int, :int], :int\n  ffi_func :mx_cuda, [], :int\nend\n' > spinel-mathx/mathx.rb
mkdir -p featconsumer/bin
printf '[package]\nname = "featconsumer"\n' > featconsumer/spin.toml
printf 'require "mathx"\nputs Mathx.mx_cuda\n' > featconsumer/bin/app.rb
cd featconsumer
"$SPIN" add mathx --path ../spinel-mathx --features cuda >/dev/null
grep -q 'features = \["cuda"\]' spin.toml || fail "spin add --features didn't record the enablement"
OUT=$("$SPIN" run 2>&1)
expect "feature-enabled native run" "999" "$(echo "$OUT" | tail -1)"
find "$XDG_CACHE_HOME/spin/native" -name 'libmx-cuda.a' | grep -q . || fail "enabled feature entry didn't build"

echo "spin-e2e: ALL GREEN"
