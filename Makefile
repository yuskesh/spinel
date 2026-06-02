# Spinel AOT Compiler - Makefile
#
# Usage:
#   make              Build everything (parser + bootstrap compiler)
#   make parse        Build C parser only
#   make bootstrap    Bootstrap the compiler backend
#   make test         Run feature tests (requires bootstrap first)
#   make fast-test    Force feature tests with cheaper C optimization
#   make bench        Run benchmarks (requires bootstrap first)
#   make clean        Remove built binaries

# Machine-local overrides (gitignored). Lets a developer point the
# bootstrap at an alternative Ruby -- e.g. a YJIT-enabled miniruby --
# and set OPT/CC/etc. without editing committed defaults. Included
# before the `?=` defaults below so its values win; a command-line
# `make VAR=...` still overrides everything. Example local.mk:
#   BOOTSTRAP_RUBY = /path/to/ruby/miniruby
#   YJIT = 1
-include local.mk

CC       ?= cc
# Auto-wrap CC with sccache or ccache when present. Skip when CC is
# already wrapped — the substring guard catches both "ccache" and
# "sccache" since "ccache" is a substring of "sccache", so CI's
# `CC=sccache <cc>` env stays untouched. NO_CCACHE=1 opts out.
# `override` is required so that `make CC=gcc` (command-line CC) is
# also wrapped — without it, command-line CC takes precedence over
# Makefile assignments and the wrap is silently ignored.
ifeq (,$(findstring ccache,$(CC)))
ifeq (,$(NO_CCACHE))
  CCACHE_BIN := $(shell command -v sccache 2>/dev/null || command -v ccache 2>/dev/null)
  ifneq (,$(CCACHE_BIN))
    override CC := $(CCACHE_BIN) $(CC)
  endif
endif
endif
# `-Wno-alloc-size-larger-than` silences a gcc 13+ paranoia warning that
# fires on the inlined `h->cap *= 2` in sp_*Hash_grow when the inliner
# can't bound h->cap. The pattern is bounded in practice (cap doubles
# from a small power of two until memory runs out, never approaches
# 2^60), but gcc tracks signed-overflow UB conservatively. Clang
# doesn't have that warning; without `-Wno-unknown-warning-option`
# first, clang would turn the unknown -Wno- into a -Werror failure on
# every cc invocation in `make test` / `make bench`.
# Optimization level for the C compiles driven by the test/bench
# harness (each .rb gets parsed → codegen → cc'd → run). CI overrides
# this to -O0 to cut Windows cc time substantially; locally -O2 keeps
# the test binaries reasonable-speed for quicker run-after-compile.
OPT     ?= -O2
CFLAGS   = $(OPT) -Wno-all -Wno-unknown-warning-option -Wno-alloc-size-larger-than
# Bootstrap-only flags: spinel_codegen runs on the developer's machine
# only, so we can use -O3 -flto for ~5-10% extra wall-clock without
# constraining users (whose generated C is built with plain CFLAGS).
# Override with LTO=0 on toolchains without LTO support.
LTO     ?= 1
ifeq ($(LTO),1)
  BOOTSTRAP_CFLAGS = -O3 -flto=auto -Wno-all
else
  BOOTSTRAP_CFLAGS = $(CFLAGS)
endif
# Per-function sections allow the linker to strip unused bigint/regexp
# functions from the final binary (supported since GCC 2.7 / binutils 2.17).
SEC_FLAGS = -ffunction-sections -fdata-sections
# Apple ld64 spells dead-code stripping --dead_strip; GNU ld uses --gc-sections.
ifeq ($(shell uname -s),Darwin)
  GC_FLAGS = -Wl,-dead_strip
else
  GC_FLAGS = -Wl,--gc-sections
endif

# MinGW gcc appends .exe to its output filename; reflect that in target
# names so Make's dependency tracking and install/clean match reality.
# Windows (MinGW) default stack is also only 1MB — far too small for the
# deeply recursive bootstrap compile (~75k frames of AST traversal).
ifeq ($(OS),Windows_NT)
  EXE = .exe
  LDFLAGS += -Wl,--stack,67108864
endif

# `timeout` is GNU coreutils — present by default on Linux but missing
# on macOS unless `brew install coreutils` is run (where it's named
# `gtimeout`). Without it test/bench would chain-fail at the very first
# `timeout` call with exit 127, reporting 0 pass / 0 fail. Detect both
# names; if neither is found, run without time limits (slow benches
# won't be auto-killed, but otherwise everything works).
TIMEOUT_BIN := $(shell command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null)
TIMEOUT10 := $(if $(TIMEOUT_BIN),$(TIMEOUT_BIN) 10,)
TIMEOUT60 := $(if $(TIMEOUT_BIN),$(TIMEOUT_BIN) 60,)

# Default -j to logical CPU count when MAKEFLAGS doesn't already
# carry a -j flag. The combined guard catches the three forms a user
# can supply -j in (per the GNU Make manual): `-j N` / `-jN` via
# filter, the short-flag-cluster form like `-kj` via findstring on
# the first word (the leading `-` makes firstword non-empty when
# MAKEFLAGS starts blank), and Make 4.x's `--jobserver-auth=…` long
# form. Note: GNU Make 3.81 (macOS system make) reports MAKEFLAGS
# as empty at parse time, so the guard always fires there; users on
# 3.81 wanting to override should pass `MAKEFLAGS=-jN make …`.
ifeq (,$(filter -j%,$(MAKEFLAGS))$(findstring j,$(firstword -$(MAKEFLAGS)))$(filter --jobserver%,$(MAKEFLAGS)))
  MAKEFLAGS += -j$(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
endif

# Reference Ruby for `make test` / `make bench` output comparison.
# Defaults to `ruby` (the system CRuby), matching the historical
# behaviour. Override on the command line when a newer or differently-
# built interpreter is needed, e.g.
#
#   REF_RUBY=miniruby make test
#
# to use a freshly-built bootstrap interpreter (Ruby's `miniruby`)
# that supports newer features like the `it` block param. The
# harness falls back to `ruby` per-file if `REF_RUBY` exits non-zero
# — covers tests that `require` extension libraries (stringio, etc.)
# which the bootstrap miniruby can't load.
REF_RUBY ?= ruby

# Ruby used to run the bootstrap compiler (spinel_analyze.rb /
# spinel_codegen.rb interpret the compiler on its own source). These
# are long-running, hot-loop-heavy processes — exactly where YJIT
# amortizes its warmup — so `YJIT=1 make` adds `--yjit` here only.
# Opt-in because it requires a Ruby built with YJIT (3.3+ default-on,
# 3.1/3.2 need a YJIT-enabled build). The per-test REF_RUBY is left on
# plain `ruby`: those spawn a fresh short-lived process per file, so
# YJIT warmup never pays off before the process exits. YJIT does not
# change semantics, so the emitted IR / C is identical either way.
BOOTSTRAP_RUBY ?= ruby
ifeq ($(YJIT),1)
  BOOTSTRAP_RUBY := $(BOOTSTRAP_RUBY) --yjit
endif

# Prism library: prefer vendor/prism (fetched via `make deps`), then
# fall back to the Prism gem if one is installed. Override by setting
# PRISM_DIR=/path/to/prism on the command line.
PRISM_VERSION ?= 1.9.0
ifneq ($(wildcard vendor/prism/include/prism.h),)
  PRISM_DIR ?= vendor/prism
else
  PRISM_DIR ?= $(shell ruby -rprism -e 'puts $$LOADED_FEATURES.grep(/prism/).first.sub(%r{/lib/.*}, "")' 2>/dev/null)
endif

PRISM_INC    = $(PRISM_DIR)/include
PRISM_SRC    = $(wildcard $(PRISM_DIR)/src/*.c) $(wildcard $(PRISM_DIR)/src/util/*.c)
PRISM_OBJ    = $(patsubst $(PRISM_DIR)/src/%.c,build/prism/%.o,$(PRISM_SRC))
PRISM_LIB    = build/libprism.a

# rbs C parser. Fetched via `make deps` from rubygems.org (the rbs
# gem bundles its standalone C parser under src/ + include/, the same
# way prism does). Consumed by spinel_rbs_extract to produce a seed
# file for spinel_analyze when --rbs is passed.
RBS_VERSION ?= 4.0.1
RBS_DIR      = vendor/rbs
RBS_INC      = $(RBS_DIR)/include
RBS_SRC      = $(wildcard $(RBS_DIR)/src/*.c) $(wildcard $(RBS_DIR)/src/util/*.c)
RBS_OBJ      = $(patsubst $(RBS_DIR)/src/%.c,build/rbs/%.o,$(RBS_SRC))
RBS_LIB      = build/librbs.a

CODEGEN_STAMP := build/stamps/spinel_codegen.rb.stamp
ANALYZE_STAMP := build/stamps/spinel_analyze.rb.stamp
NODE_TABLE_LOADER_STAMP := build/stamps/node_table_loader.rb.stamp
COMPILER_HELPERS_STAMP := build/stamps/compiler_helpers.rb.stamp
PARSE_STAMP   := build/stamps/spinel_parse.c.stamp

.PHONY: all parse bootstrap codegen rbs_extract rbs-test regen-rbs-expected test retest fast-test clean-test-results regen-expected bench optcarrot clean install uninstall deps FORCE

# `make all` includes spinel_rbs_extract when vendor/rbs has been
# fetched (via `make deps`). Without vendor/rbs the extractor is
# silently omitted -- spinel still works; `spinel --rbs DIR` then
# becomes a no-op (warns and proceeds without seeds).
ifneq ($(wildcard $(RBS_INC)/rbs/parser.h),)
  RBS_EXTRACT_TARGET = spinel_rbs_extract$(EXE)
else
  RBS_EXTRACT_TARGET =
endif

all: parse regexp spinel_analyze$(EXE) spinel_codegen$(EXE) $(RBS_EXTRACT_TARGET)

# ---- Dependencies ----
# Clone Prism into vendor/prism at the pinned version. Run this once
# after cloning Spinel if you don't have the Prism gem installed.
deps: vendor/prism/include/prism/diagnostic.h vendor/rbs/include/rbs/parser.h

# Download the pre-built Prism gem from rubygems.org and extract its C
# sources. We use the .gem tarball instead of a git clone because it
# ships with the generated headers (diagnostic.h, etc.) already in
# place — no rake/bundler needed.
vendor/prism/include/prism/diagnostic.h:
	@mkdir -p vendor/prism
	@echo "Fetching prism v$(PRISM_VERSION) from rubygems.org..."
	curl -sL -o /tmp/prism-$(PRISM_VERSION).gem https://rubygems.org/gems/prism-$(PRISM_VERSION).gem
	@tmpdir=$$(mktemp -d); \
	 tar -xf /tmp/prism-$(PRISM_VERSION).gem -C $$tmpdir data.tar.gz; \
	 tar -xzf $$tmpdir/data.tar.gz -C vendor/prism; \
	 rm -rf $$tmpdir /tmp/prism-$(PRISM_VERSION).gem
	@test -f $@ && echo "prism v$(PRISM_VERSION) ready at vendor/prism"

# Same shape as the prism fetch above: download the rbs gem from
# rubygems.org and extract its bundled C parser into vendor/rbs.
# The gem ships src/ + include/ (the same files used to build the
# Rust ruby-rbs binding and the Ruby C-extension) which is exactly
# what spinel_rbs_extract needs.
vendor/rbs/include/rbs/parser.h:
	@mkdir -p vendor/rbs
	@echo "Fetching rbs v$(RBS_VERSION) from rubygems.org..."
	curl -sL -o /tmp/rbs-$(RBS_VERSION).gem https://rubygems.org/gems/rbs-$(RBS_VERSION).gem
	@tmpdir=$$(mktemp -d); \
	 tar -xf /tmp/rbs-$(RBS_VERSION).gem -C $$tmpdir data.tar.gz; \
	 tar -xzf $$tmpdir/data.tar.gz -C vendor/rbs; \
	 rm -rf $$tmpdir /tmp/rbs-$(RBS_VERSION).gem
	@test -f $@ && echo "rbs v$(RBS_VERSION) ready at vendor/rbs"

# If PRISM_DIR ended up empty (no vendor/prism, no gem), halt with a
# clear message before trying to build anything that needs it.
ifeq ($(PRISM_DIR),)
parse bootstrap codegen regexp all: prism-missing
prism-missing:
	@echo "Error: Prism not found."; \
	 echo "  Run 'make deps' to fetch libprism into vendor/prism,"; \
	 echo "  or install the prism gem (gem install prism),"; \
	 echo "  or set PRISM_DIR=/path/to/prism manually."; \
	 exit 1
endif

# ---- Prism static library ----

build/libprism.a: $(PRISM_OBJ)
	@mkdir -p build
	ar rcs $@ $^

build/prism/%.o: $(PRISM_DIR)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) -c -O2 -I$(PRISM_INC) -I$(PRISM_DIR)/src $< -o $@

# ---- rbs static library ----

build/librbs.a: $(RBS_OBJ)
	@mkdir -p build
	ar rcs $@ $^

build/rbs/%.o: $(RBS_DIR)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) -c -O2 -Wno-all -I$(RBS_INC) -I$(RBS_DIR)/src $< -o $@

# ---- Content stamps ----
# Content stamps: downstream rules depend on `build/stamps/foo.stamp`
# instead of `foo` directly, so `touch foo` (or `git checkout` of an
# identical version) doesn't invalidate bootstrap/test targets.
#
# The stamp rule intentionally does not list `%` as a normal prerequisite:
# if it did, a same-content but newer source would make GNU make consider
# the stamp out-of-date on every run when cmp leaves the stamp untouched.
# FORCE lets us cheaply re-check content each invocation, while downstream
# targets only rebuild when cp actually advances the stamp's mtime.
build/stamps:
	@mkdir -p $@

FORCE:

build/stamps/%.stamp: FORCE | build/stamps
	@cmp -s $* $@ 2>/dev/null || cp $* $@

# Without .PRECIOUS make would delete these as pattern-rule
# intermediates, recreating them with fresh mtimes next run.
.PRECIOUS: build/stamps/%.stamp

# ---- C Parser ----

parse: spinel_parse$(EXE)

spinel_parse$(EXE): $(PARSE_STAMP) $(PRISM_LIB)
	$(CC) $(CFLAGS) -I$(PRISM_INC) spinel_parse.c $(PRISM_LIB) -lm -o $@

# ---- RBS extractor ----
# Reads sig/**/*.rbs, emits the seed-file format spinel_analyze
# consumes when invoked with `spinel --rbs DIR`.

ifeq ($(wildcard $(RBS_INC)/rbs/parser.h),)
rbs_extract: rbs-missing
rbs-missing:
	@echo "Error: rbs C parser not found at $(RBS_INC)/rbs/parser.h."; \
	 echo "  Run 'make deps' to fetch it from rubygems.org into vendor/rbs."; \
	 exit 1
else
rbs_extract: spinel_rbs_extract$(EXE)

spinel_rbs_extract$(EXE): spinel_rbs_extract.c $(RBS_LIB)
	$(CC) $(CFLAGS) -I$(RBS_INC) spinel_rbs_extract.c $(RBS_LIB) -o $@
endif

# ---- Runtime library (regexp + bigint) ----

RE_SRC = lib/regexp/re_compile.c lib/regexp/re_exec.c lib/regexp/re_utf8.c
RE_OBJ = $(patsubst lib/regexp/%.c,build/regexp/%.o,$(RE_SRC))

build/regexp/%.o: lib/regexp/%.c lib/regexp/re_internal.h
	@mkdir -p build/regexp
	$(CC) -c -O2 $(SEC_FLAGS) -Ilib/regexp $< -o $@

build/sp_bigint.o: lib/sp_bigint.c lib/sp_bigint.h lib/mruby_shim.h
	@mkdir -p build
	$(CC) -c -O2 -Wno-all $(SEC_FLAGS) -Ilib lib/sp_bigint.c -o build/sp_bigint.o

build/sp_crypto.o: lib/sp_crypto.c lib/sp_crypto.h
	@mkdir -p build
	$(CC) -c -O2 -Wno-all $(SEC_FLAGS) -Ilib lib/sp_crypto.c -o build/sp_crypto.o

build/sp_pack.o: lib/sp_pack.c lib/mruby_shim.h
	@mkdir -p build
	$(CC) -c -O2 -Wno-all $(SEC_FLAGS) -Ilib lib/sp_pack.c -o build/sp_pack.o

build/sp_strscan.o: lib/sp_strscan.c lib/mruby_shim.h
	@mkdir -p build
	$(CC) -c -O2 -Wno-all $(SEC_FLAGS) -Ilib -Ilib/regexp lib/sp_strscan.c -o build/sp_strscan.o

build/sp_time.o: lib/sp_time.c lib/sp_time.h
	@mkdir -p build
	$(CC) -c -O2 -Wno-all $(SEC_FLAGS) -Ilib lib/sp_time.c -o build/sp_time.o

build/sp_core.o: lib/sp_core.c lib/sp_core.h
	@mkdir -p build
	$(CC) -c -O2 -Wno-all $(SEC_FLAGS) -Ilib lib/sp_core.c -o build/sp_core.o

build/sp_net.o: lib/sp_net.c lib/sp_net.h
	@mkdir -p build
	$(CC) -c -O2 -Wno-all $(SEC_FLAGS) -Ilib lib/sp_net.c -o build/sp_net.o

SP_RT_LIB = lib/libspinel_rt.a

$(SP_RT_LIB): $(RE_OBJ) build/sp_bigint.o build/sp_crypto.o build/sp_pack.o build/sp_strscan.o build/sp_time.o build/sp_core.o build/sp_net.o
	ar rcs $@ $^

regexp: $(SP_RT_LIB)

# ---- Build the codegen binary (fast path) ----
# Build both binaries via CRuby (the bootstrap "round 1"). Each
# spinel_<phase>.rb is its own ~20K-line source; we compile each
# through the same parse → analyze → codegen → cc pipeline. Use
# `make bootstrap` for the full self-hosting fixed-point check.

codegen: spinel_analyze$(EXE) spinel_codegen$(EXE)

# Round 1 pipeline split into per-stage rules. Previously the
# `spinel_<phase>$(EXE)` recipe produced its AST/IR/C1 outputs as
# side effects, which left those files without proper Make rules.
# Under parallel make that races: `build/codegen2.ir` (declared to
# depend on `build/codegen.ast`) could be scheduled before the
# spinel_codegen recipe had refreshed the AST, so it read a stale
# AST while later steps used the fresh one. The bid mismatch then
# corrupted scope tables in the bootstrap fixpoint output. See #620.

build/analyze.ast: $(ANALYZE_STAMP) $(NODE_TABLE_LOADER_STAMP) $(COMPILER_HELPERS_STAMP) spinel_parse$(EXE)
	./spinel_parse$(EXE) spinel_analyze.rb build/analyze.ast

build/codegen.ast: $(CODEGEN_STAMP) $(NODE_TABLE_LOADER_STAMP) $(COMPILER_HELPERS_STAMP) spinel_parse$(EXE)
	./spinel_parse$(EXE) spinel_codegen.rb build/codegen.ast

build/analyze.ir: build/analyze.ast $(ANALYZE_STAMP) $(NODE_TABLE_LOADER_STAMP) $(COMPILER_HELPERS_STAMP)
	$(BOOTSTRAP_RUBY) spinel_analyze.rb build/analyze.ast build/analyze.ir

build/codegen.ir: build/codegen.ast $(ANALYZE_STAMP) $(NODE_TABLE_LOADER_STAMP) $(COMPILER_HELPERS_STAMP)
	$(BOOTSTRAP_RUBY) spinel_analyze.rb build/codegen.ast build/codegen.ir

build/analyze1.c: build/analyze.ast build/analyze.ir $(CODEGEN_STAMP) $(NODE_TABLE_LOADER_STAMP) $(COMPILER_HELPERS_STAMP)
	$(BOOTSTRAP_RUBY) spinel_codegen.rb build/analyze.ast build/analyze.ir build/analyze1.c

build/codegen1.c: build/codegen.ast build/codegen.ir $(CODEGEN_STAMP) $(NODE_TABLE_LOADER_STAMP) $(COMPILER_HELPERS_STAMP)
	$(BOOTSTRAP_RUBY) spinel_codegen.rb build/codegen.ast build/codegen.ir build/codegen1.c

spinel_analyze$(EXE): build/analyze1.c $(SP_RT_LIB)
	$(CC) $(BOOTSTRAP_CFLAGS) -Ilib build/analyze1.c $(SP_RT_LIB) $(LDFLAGS) -lm -o spinel_analyze$(EXE)

spinel_codegen$(EXE): build/codegen1.c $(SP_RT_LIB)
	$(CC) $(BOOTSTRAP_CFLAGS) -Ilib build/codegen1.c $(SP_RT_LIB) $(LDFLAGS) -lm -o spinel_codegen$(EXE)

# ---- Self-hosting verification ----
# After CRuby builds spinel_{analyze,codegen}, run them on each
# source file to produce a "round 2" pair of binaries. Then run
# bin2_{analyze,codegen} on each source again to produce "round 3"
# outputs and assert byte equality. The fixpoint covers both the
# analysis IR and the emitted C, so any drift in either phase is
# caught.

# Round 2: gen1 binaries (spinel_{analyze,codegen}) compile each .rb
# source into a fresh `bin2_*` binary. The analyze and codegen sources
# are independent, so the two pipelines are split into per-binary
# targets that `make -j` can run in parallel. Same for the round 3
# verify step.

build/analyze2.ir: build/analyze.ast spinel_analyze$(EXE)
	./spinel_analyze$(EXE) build/analyze.ast build/analyze2.ir

build/analyze2.c: build/analyze.ast build/analyze2.ir spinel_codegen$(EXE)
	./spinel_codegen$(EXE) build/analyze.ast build/analyze2.ir build/analyze2.c

build/bin2_analyze$(EXE): build/analyze2.c $(SP_RT_LIB)
	$(CC) $(BOOTSTRAP_CFLAGS) -Ilib build/analyze2.c $(SP_RT_LIB) $(LDFLAGS) -lm -o build/bin2_analyze$(EXE)

build/codegen2.ir: build/codegen.ast spinel_analyze$(EXE)
	./spinel_analyze$(EXE) build/codegen.ast build/codegen2.ir

build/codegen2.c: build/codegen.ast build/codegen2.ir spinel_codegen$(EXE)
	./spinel_codegen$(EXE) build/codegen.ast build/codegen2.ir build/codegen2.c

build/bin2_codegen$(EXE): build/codegen2.c $(SP_RT_LIB)
	$(CC) $(BOOTSTRAP_CFLAGS) -Ilib build/codegen2.c $(SP_RT_LIB) $(LDFLAGS) -lm -o build/bin2_codegen$(EXE)

build/analyze3.ir: build/analyze.ast build/bin2_analyze$(EXE)
	./build/bin2_analyze$(EXE) build/analyze.ast build/analyze3.ir

build/analyze3.c: build/analyze.ast build/analyze3.ir build/bin2_codegen$(EXE)
	./build/bin2_codegen$(EXE) build/analyze.ast build/analyze3.ir build/analyze3.c

build/codegen3.ir: build/codegen.ast build/bin2_analyze$(EXE)
	./build/bin2_analyze$(EXE) build/codegen.ast build/codegen3.ir

build/codegen3.c: build/codegen.ast build/codegen3.ir build/bin2_codegen$(EXE)
	./build/bin2_codegen$(EXE) build/codegen.ast build/codegen3.ir build/codegen3.c

bootstrap: build/analyze3.c build/codegen3.c
	@diff build/analyze2.ir build/analyze3.ir > /dev/null && echo "analyze.rb: IR fixpoint OK" || (echo "BOOTSTRAP FAILED: analyze.rb IR diverged" && exit 1)
	@diff build/analyze2.c  build/analyze3.c  > /dev/null && echo "analyze.rb: C fixpoint OK"  || (echo "BOOTSTRAP FAILED: analyze.rb C diverged"  && exit 1)
	@diff build/codegen2.ir build/codegen3.ir > /dev/null && echo "codegen.rb: IR fixpoint OK" || (echo "BOOTSTRAP FAILED: codegen.rb IR diverged" && exit 1)
	@diff build/codegen2.c  build/codegen3.c  > /dev/null && echo "codegen.rb: C fixpoint OK"  || (echo "BOOTSTRAP FAILED: codegen.rb C diverged"  && exit 1)
	cp build/bin2_analyze$(EXE) spinel_analyze$(EXE)
	cp build/bin2_codegen$(EXE) spinel_codegen$(EXE)

# ---- Test ----

TESTS := $(wildcard test/*.rb)
# Mode-incompatible tests: int_overflow_raises pins raise-mode semantics
# (RangeError on overflow); under --int-overflow=promote the same code
# auto-promotes to bigint and the expected output diverges by design.
ifeq ($(SPINEL_INT_OVERFLOW),promote)
TESTS := $(filter-out test/int_overflow_raises.rb,$(TESTS))
endif
# sp_net is POSIX-only; on Windows the TU compiles to stubs, so the
# sp_net smoke's output diverges. Skip it there (the POSIX surface is
# exercised by consumer suites on POSIX targets).
ifeq ($(OS),Windows_NT)
TESTS := $(filter-out test/sp_net_basic.rb,$(TESTS))
endif
TEST_TARGETS := $(patsubst test/%.rb,build/test-results/%.ok,$(TESTS))

# `make test` is incremental via mtime tracking on .ok files;
# `make retest` wipes them for a forced rerun. The rbs-test prereq
# golden-checks the RBS extractor (cheap, runs every invocation).
test: rbs-test $(TEST_TARGETS)
	@if [ -z "$(TIMEOUT_BIN)" ]; then echo "Note: no 'timeout' command found; running without time limits."; fi
	@if [ -t 1 ]; then printf '\n'; fi
	@pass=$$(grep -l '^PASS' build/test-results/*.ok 2>/dev/null | wc -l); \
	fail=$$(grep -l '^FAIL' build/test-results/*.ok 2>/dev/null | wc -l); \
	err=$$(grep -l '^ERR' build/test-results/*.ok 2>/dev/null | wc -l); \
	for f in build/test-results/*.ok; do \
	  bn=$$(basename "$$f" .ok); \
	  status=$$(cat "$$f"); \
	  if [ "$$status" = FAIL ]; then \
	    echo "FAIL: $$bn"; \
	    head -40 "$$f.diff"; \
	  elif [ "$$status" = ERR ]; then \
	    echo "ERR:  $$bn"; \
	  fi; \
	done; \
	echo "Tests: $$pass pass, $$fail fail, $$err error"; \
	if [ $$fail -ne 0 ] || [ $$err -ne 0 ]; then exit 1; fi

retest: clean-test-results
	@$(MAKE) --no-print-directory test

fast-test: clean-test-results
	@$(MAKE) --no-print-directory test OPT=-O0 LTO=0

# ---- RBS extractor golden tests ----
# spinel_rbs_extract is a pure text transform (*.rbs -> seed lines on
# stdout), so it golden-tests cleanly without Ruby at test time. Each
# test/rbs/<name>.rbs has a committed test/rbs/<name>.seed.expected;
# `rbs-test` diffs live output against it. Regenerate after intentional
# extractor changes with `make regen-rbs-expected` and commit the diff.
# Skips (rather than fails) when vendor/rbs hasn't been fetched, so a
# `make deps`-less checkout can still run the rest of `make test`.
RBS_TEST_SRCS := $(sort $(wildcard test/rbs/*.rbs))

ifeq ($(wildcard $(RBS_INC)/rbs/parser.h),)
rbs-test:
	@echo "rbs-test: skipped (vendor/rbs not fetched; run 'make deps')"
regen-rbs-expected:
	@echo "regen-rbs-expected: skipped (vendor/rbs not fetched; run 'make deps')"
else
rbs-test: spinel_rbs_extract$(EXE)
	@fail=0; n=0; \
	for f in $(RBS_TEST_SRCS); do \
	  n=$$((n+1)); \
	  exp="$${f%.rbs}.seed.expected"; \
	  if [ ! -f "$$exp" ]; then echo "rbs-test: MISSING golden $$exp"; fail=1; continue; fi; \
	  d=$$(./spinel_rbs_extract$(EXE) "$$f" 2>/dev/null | diff -u "$$exp" - 2>&1); \
	  if [ -z "$$d" ]; then \
	    if [ -t 1 ]; then printf .; fi; \
	  else \
	    echo; echo "rbs-test FAIL: $$f"; echo "$$d"; fail=1; \
	  fi; \
	done; \
	if [ -t 1 ]; then printf '\n'; fi; \
	if [ $$fail -ne 0 ]; then echo "RBS extractor tests: FAIL"; exit 1; fi; \
	echo "RBS extractor tests: $$n pass"

regen-rbs-expected: spinel_rbs_extract$(EXE)
	@for f in $(RBS_TEST_SRCS); do \
	  ./spinel_rbs_extract$(EXE) "$$f" > "$${f%.rbs}.seed.expected"; \
	  echo "regen: $${f%.rbs}.seed.expected"; \
	done
endif

# The .ok target is the test's stamp; mtime tracking gives per-test
# caching for free. Order-only spinel_parse$(EXE) / spinel_analyze$(EXE)
# / spinel_codegen$(EXE) stop a bootstrap relink from invalidating every test.
build/test-results/%.ok: test/%.rb $(SP_RT_LIB) $(CODEGEN_STAMP) $(ANALYZE_STAMP) $(NODE_TABLE_LOADER_STAMP) $(COMPILER_HELPERS_STAMP) $(PARSE_STAMP) | spinel_parse$(EXE) spinel_analyze$(EXE) spinel_codegen$(EXE)
	@mkdir -p build/test-results
	@tmpdir=$$(mktemp -d /tmp/spinel-test.XXXXXX); \
	ast=$$tmpdir/test.ast; \
	ir=$$tmpdir/test.ir; \
	cfile=$$tmpdir/test.c; \
	bin=$$tmpdir/test_bin$(EXE); \
	exp=$$tmpdir/expected; \
	act=$$tmpdir/actual; \
	args=""; \
	if [ -f "$<.args" ]; then args=$$(cat "$<.args"); fi; \
	rm -f "$@.diff"; \
	./spinel_parse$(EXE) "$<" "$$ast" 2>/dev/null && \
	./spinel_analyze$(EXE) "$$ast" "$$ir" 2>/dev/null && \
	./spinel_codegen$(EXE) "$$ast" "$$ir" "$$cfile" 2>/dev/null && \
	$(CC) $(CFLAGS) -Werror $(SEC_FLAGS) -Ilib -c "$$cfile" -o "$$cfile.o" 2>/dev/null && \
	$(CC) $(CFLAGS) "$$cfile.o" $(SP_RT_LIB) $(LDFLAGS) -lm $(GC_FLAGS) -o "$$bin" 2>/dev/null; \
	if [ $$? -eq 0 ]; then \
	  if [ -f "$<.expected" ]; then \
	    LC_ALL=C sed 's/\r$$//' "$<.expected" >"$$exp.n"; \
	  else \
	    $(TIMEOUT10) $(REF_RUBY) "$<" $$args >"$$exp" 2>/dev/null; \
	    ruby_rc=$$?; \
	    if [ $$ruby_rc -ne 0 ] && [ "$(REF_RUBY)" != "ruby" ]; then \
	      $(TIMEOUT10) ruby "$<" $$args >"$$exp" 2>/dev/null; \
	    fi; \
	    LC_ALL=C sed 's/\r$$//' "$$exp" >"$$exp.n"; \
	  fi; \
	  $(TIMEOUT10) "$$bin" $$args >"$$act" 2>/dev/null; \
	  LC_ALL=C sed 's/\r$$//' "$$act" >"$$act.n"; \
	  if cmp -s "$$exp.n" "$$act.n"; then \
	    echo PASS > "$@"; \
	    if [ -t 1 ]; then printf .; fi; \
	  else \
	    echo FAIL > "$@"; \
	    diff -u "$$exp.n" "$$act.n" > "$@.diff" 2>&1 || true; \
	    if [ -t 1 ]; then printf F; fi; \
	  fi; \
	else \
	  echo ERR > "$@"; \
	  if [ -t 1 ]; then printf E; fi; \
	fi; \
	rm -rf "$$tmpdir"

clean-test-results:
	@rm -rf build/test-results

# ---- Expected-output regeneration ----
# Capture each test's reference Ruby output into test/<name>.rb.expected.
# Once committed, the test target uses the .expected file directly and
# skips the per-test ruby invocation — useful in CI where ruby's startup
# (especially mingw64 ruby on Windows) adds up across hundreds of tests.
# Regenerate after adding or modifying tests; commit the result.

EXPECTED_FILES := $(patsubst test/%.rb,test/%.rb.expected,$(TESTS))

regen-expected: $(EXPECTED_FILES)

test/%.rb.expected: test/%.rb
	@args=""; \
	if [ -f "$<.args" ]; then args=$$(cat "$<.args"); fi; \
	$(TIMEOUT10) $(REF_RUBY) $< $$args >$@.tmp 2>/dev/null; \
	rc=$$?; \
	if [ $$rc -ne 0 ] && [ "$(REF_RUBY)" != "ruby" ]; then \
	  $(TIMEOUT10) ruby $< $$args >$@.tmp 2>/dev/null; \
	  rc=$$?; \
	fi; \
	if [ $$rc -ne 0 ]; then \
	  echo "regen-expected: $< failed (rc=$$rc); skipping" >&2; \
	  rm -f $@.tmp; \
	else \
	  LC_ALL=C sed 's/\r$$//' $@.tmp > $@; \
	  rm -f $@.tmp; \
	fi

bench: spinel_parse$(EXE) $(SP_RT_LIB) spinel_analyze$(EXE) spinel_codegen$(EXE)
	@if [ -z "$(TIMEOUT_BIN)" ]; then echo "Note: no 'timeout' command found; running without time limits."; fi
	@total=$$(ls benchmark/*.rb | wc -l); \
	if [ -t 1 ]; then tty=1; else tty=0; fi; \
	pass=0; fail=0; err=0; skip=0; i=0; \
	for f in benchmark/*.rb; do \
	  i=$$((i+1)); \
	  bn=$$(basename "$$f" .rb); \
	  if [ "$$tty" = 1 ]; then printf '\r\033[K  [%d/%d] %s' "$$i" "$$total" "$$bn"; fi; \
	  $(TIMEOUT10) ./spinel_parse$(EXE) "$$f" /tmp/_sp_b.ast 2>/dev/null && \
	  $(TIMEOUT10) ./spinel_analyze$(EXE) /tmp/_sp_b.ast /tmp/_sp_b.ir 2>/dev/null && \
	  $(TIMEOUT10) ./spinel_codegen$(EXE) /tmp/_sp_b.ast /tmp/_sp_b.ir /tmp/_sp_b.c 2>/dev/null && \
	  $(CC) $(CFLAGS) -Werror $(SEC_FLAGS) -Ilib -c /tmp/_sp_b.c -o /tmp/_sp_b.c.o 2>/dev/null && \
	  $(CC) $(CFLAGS) /tmp/_sp_b.c.o $(SP_RT_LIB) $(LDFLAGS) -lm $(GC_FLAGS) -o /tmp/_sp_b_bin$(EXE) 2>/dev/null; \
	  if [ $$? -eq 0 ]; then \
	    $(TIMEOUT60) $(REF_RUBY) "$$f" >/tmp/_sp_b_exp 2>/dev/null; \
	    ruby_rc=$$?; \
	    if [ $$ruby_rc -ne 0 ] && [ "$(REF_RUBY)" != "ruby" ]; then \
	      $(TIMEOUT60) ruby "$$f" >/tmp/_sp_b_exp 2>/dev/null; \
	      ruby_rc=$$?; \
	    fi; \
	    if [ $$ruby_rc -eq 124 ]; then \
	      if [ "$$tty" = 1 ]; then printf '\r\033[K'; fi; \
	      echo "SKIP: $$bn (ruby timeout)"; skip=$$((skip+1)); \
	    else \
	      $(TIMEOUT60) /tmp/_sp_b_bin$(EXE) >/tmp/_sp_b_act 2>/dev/null; \
	      LC_ALL=C sed 's/\r$$//' /tmp/_sp_b_exp >/tmp/_sp_b_exp.n; \
	      LC_ALL=C sed 's/\r$$//' /tmp/_sp_b_act >/tmp/_sp_b_act.n; \
	      if cmp -s /tmp/_sp_b_exp.n /tmp/_sp_b_act.n; then \
	        pass=$$((pass+1)); \
	      else \
	        if [ "$$tty" = 1 ]; then printf '\r\033[K'; fi; \
	        echo "FAIL: $$bn"; \
	        diff -u /tmp/_sp_b_exp.n /tmp/_sp_b_act.n 2>&1 | head -40; \
	        fail=$$((fail+1)); \
	      fi; \
	    fi; \
	  else \
	    if [ "$$tty" = 1 ]; then printf '\r\033[K'; fi; \
	    echo "ERR:  $$bn"; err=$$((err+1)); \
	  fi; \
	done; \
	if [ "$$tty" = 1 ]; then printf '\r\033[K'; fi; \
	rm -f /tmp/_sp_b.ast /tmp/_sp_b.ir /tmp/_sp_b.c /tmp/_sp_b.c.o /tmp/_sp_b_bin$(EXE) /tmp/_sp_b_exp /tmp/_sp_b_act /tmp/_sp_b_exp.n /tmp/_sp_b_act.n; \
	echo "Benchmarks: $$pass pass, $$fail fail, $$err error, $$skip skip"; \
	if [ $$fail -ne 0 ] || [ $$err -ne 0 ]; then exit 1; fi

# ---- Optcarrot integration test ----
#
# End-to-end pipeline: clone optcarrot's `experiment/spinel` branch,
# pack `lib/optcarrot/*.rb` into a single Ruby file via the upstream
# `tools/pack-for-spinel.rb`, compile through spinel, run the
# resulting binary against `examples/Lan_Master.nes`, and verify the
# output contains `fps: <num>` and `checksum: 59662` (the canonical
# 180-frame checksum for `--benchmark`).

OPTCARROT_DIR  := build/optcarrot
OPTCARROT_REPO := https://github.com/mame/optcarrot.git
OPTCARROT_BRANCH := experiment/spinel

optcarrot: spinel_parse$(EXE) $(SP_RT_LIB) spinel_analyze$(EXE) spinel_codegen$(EXE)
	@if [ ! -d $(OPTCARROT_DIR) ]; then \
	  git clone --depth=1 --branch=$(OPTCARROT_BRANCH) $(OPTCARROT_REPO) $(OPTCARROT_DIR); \
	fi
	@ruby $(OPTCARROT_DIR)/tools/pack-for-spinel.rb > build/optcarrot-single.rb
	@./spinel --int-overflow=wrap build/optcarrot-single.rb -o build/optcarrot-single
	@out=$$($(TIMEOUT60) ./build/optcarrot-single 2>&1); \
	echo "$$out"; \
	if echo "$$out" | grep -qE "^fps: [0-9.]+$$" && echo "$$out" | grep -q "^checksum: 59662$$"; then \
	  echo "Optcarrot: OK"; \
	else \
	  echo "Optcarrot: FAIL — expected 'fps: <num>' and 'checksum: 59662'"; \
	  exit 1; \
	fi

# ---- Install ----

PREFIX   ?= /usr/local
SPNLDIR   = $(PREFIX)/lib/spinel

install: all
	install -d $(SPNLDIR)/lib
	install -m 755 spinel                $(SPNLDIR)/
	install -m 755 spinel_parse$(EXE)    $(SPNLDIR)/
	install -m 755 spinel_analyze$(EXE)  $(SPNLDIR)/
	install -m 755 spinel_codegen$(EXE)  $(SPNLDIR)/
	install -m 644 node_table_loader.rb  $(SPNLDIR)/
	install -m 644 spinel_analyze.rb     $(SPNLDIR)/
	install -m 644 spinel_codegen.rb     $(SPNLDIR)/
	install -m 644 lib/libspinel_rt.a    $(SPNLDIR)/lib/
	install -m 644 lib/sp_runtime.h      $(SPNLDIR)/lib/
	install -m 644 lib/sp_core.h         $(SPNLDIR)/lib/
	install -m 644 lib/sp_time.h         $(SPNLDIR)/lib/
	install -m 644 lib/sp_net.h          $(SPNLDIR)/lib/
	install -m 644 lib/*.rb              $(SPNLDIR)/lib/
	install -d $(PREFIX)/bin
	ln -sf $(SPNLDIR)/spinel $(PREFIX)/bin/spinel

uninstall:
	rm -f $(PREFIX)/bin/spinel
	rm -rf $(SPNLDIR)

# ---- Clean ----

clean:
	rm -rf build/
	rm -f spinel_parse$(EXE) spinel_analyze$(EXE) spinel_codegen$(EXE)
