# Spinel AOT Compiler - Makefile
#
# Usage:
#   make              Build the C compiler (runtime + spinel + tools)
#   make test         Run the feature tests (always a fresh run)
#   make bench        Run benchmarks vs CRuby
#   make optcarrot    End-to-end optcarrot integration test
#   make legacy       Build the legacy Ruby compiler (delegates to legacy/)
#   make bootstrap    Legacy self-host fixpoint check (delegates to legacy/)
#   make check        Fast pre-commit: rebuild + tests
#   make gate         Full pre-push: test || bench || optcarrot
#   make clean        Remove built binaries
#
# The legacy Ruby compiler (legacy/) is built ONLY by the explicit `make legacy`
# / `make bootstrap` delegators or by `cd legacy && make`; the C compiler is
# master and a plain `make` / `make gate` never compiles it.

# Shared toolchain configuration (CC wrapping, CFLAGS, stamps, …).
include common.mk

# Prism library: prefer vendor/prism (fetched via `make deps`), then fall
# back to the Prism gem if one is installed. Override with PRISM_DIR=…
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

# rbs C parser. Fetched via `make deps` from rubygems.org. Consumed by
# spinel_rbs_extract to produce a seed file for the analyzer.
RBS_VERSION ?= 4.0.1
RBS_DIR      = vendor/rbs
RBS_INC      = $(RBS_DIR)/include
RBS_SRC      = $(wildcard $(RBS_DIR)/src/*.c) $(wildcard $(RBS_DIR)/src/util/*.c)
RBS_OBJ      = $(patsubst $(RBS_DIR)/src/%.c,build/rbs/%.o,$(RBS_SRC))
RBS_LIB      = build/librbs.a

.PHONY: all parse legacy bootstrap codegen regexp rbs_extract rbs-test rbs-seed-test \
        analyze-fail-test test test-run clean-test-results regen-rbs-expected \
        regen-expected regen-expected-err bench optcarrot gate check gate-legs gate-test gate-bench \
        gate-optcarrot clean install uninstall deps tools

# `make all` includes the RBS extractor when vendor/rbs has been fetched
# (via `make deps`); without it the extractor is silently omitted. Built under
# build/ like other intermediates; rbs-seed-test copies it beside $(SPINEL),
# where main.c looks for it as a sibling at runtime.
RBS_EXTRACT_BIN = build/spinel_rbs_extract
ifneq ($(wildcard $(RBS_INC)/rbs/parser.h),)
  RBS_EXTRACT_TARGET = $(RBS_EXTRACT_BIN)
else
  RBS_EXTRACT_TARGET =
endif

# The single Spinel binary (compiler + cc driver). Defined here, before the
# `all` rule, because a rule's prerequisites are expanded as it is read.
# Built into bin/ alongside the companion tools (spinel-doctor, ...); bin/ sits
# beside lib/ so the binary resolves its runtime lib via ../lib, same as before.
SPINEL = bin/spinel

all: regexp $(SPINEL) $(RBS_EXTRACT_TARGET) tools

# ---- Dependencies ----
deps: vendor/prism/include/prism/diagnostic.h vendor/rbs/include/rbs/parser.h

# Download the pre-built Prism gem from rubygems.org and extract its C
# sources (the .gem ships the generated headers — no rake/bundler needed).
vendor/prism/include/prism/diagnostic.h:
	@mkdir -p vendor/prism
	@echo "Fetching prism v$(PRISM_VERSION) from rubygems.org..."
	curl -sL -o /tmp/prism-$(PRISM_VERSION).gem https://rubygems.org/gems/prism-$(PRISM_VERSION).gem
	@tmpdir=$$(mktemp -d); \
	 tar -xf /tmp/prism-$(PRISM_VERSION).gem -C $$tmpdir data.tar.gz; \
	 tar -xzf $$tmpdir/data.tar.gz -C vendor/prism; \
	 rm -rf $$tmpdir /tmp/prism-$(PRISM_VERSION).gem
	@test -f $@ && echo "prism v$(PRISM_VERSION) ready at vendor/prism"

# Same shape: download the rbs gem and extract its bundled C parser.
vendor/rbs/include/rbs/parser.h:
	@mkdir -p vendor/rbs
	@echo "Fetching rbs v$(RBS_VERSION) from rubygems.org..."
	curl -sL -o /tmp/rbs-$(RBS_VERSION).gem https://rubygems.org/gems/rbs-$(RBS_VERSION).gem
	@tmpdir=$$(mktemp -d); \
	 tar -xf /tmp/rbs-$(RBS_VERSION).gem -C $$tmpdir data.tar.gz; \
	 tar -xzf $$tmpdir/data.tar.gz -C vendor/rbs; \
	 rm -rf $$tmpdir /tmp/rbs-$(RBS_VERSION).gem
	@test -f $@ && echo "rbs v$(RBS_VERSION) ready at vendor/rbs"

# If PRISM_DIR ended up empty (no vendor/prism, no gem), halt with a clear
# message before trying to build anything that needs it.
ifeq ($(PRISM_DIR),)
parse codegen regexp all: prism-missing
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

# ---- C Parser ----

parse: legacy/spinel_parse

legacy/spinel_parse: legacy/spinel_parse.c $(PRISM_LIB)
	$(CC) $(CFLAGS) -I$(PRISM_INC) legacy/spinel_parse.c $(PRISM_LIB) -lm -o $@

# ---- C compiler (src/) ----
# The single-binary C reimplementation of the analyzer + code generator.
# Links src/spinel_parse.c, the library copy of the Prism walk (no main();
# exposes sp_parse_file_to_text). The standalone-with-main copy used by the
# legacy `parse` target lives in legacy/spinel_parse.c.
# `spinel` is the single binary: it emits C and then drives cc to link it.
# (SPINEL itself is defined above, just before the `all` target.)

SPINEL_HDRS = src/node_table.h src/codegen.h src/codegen_internal.h src/types.h src/compiler.h src/analyze.h src/analyze_internal.h
SPINEL_OBJ  = build/csrc/node_table.o build/csrc/types.o build/csrc/compiler.o \
               build/csrc/analyze.o build/csrc/analyze_util.o build/csrc/analyze_infer.o \
               build/csrc/analyze_scope.o build/csrc/analyze_pass.o build/csrc/codegen.o build/csrc/codegen_util.o \
               build/csrc/codegen_fold.o build/csrc/codegen_call.o \
               build/csrc/codegen_expr.o build/csrc/codegen_stmt.o build/csrc/main.o

build/csrc:
	@mkdir -p build/csrc

build/csrc/%.o: src/%.c $(SPINEL_HDRS) | build/csrc
	$(CC) $(CFLAGS) -Isrc -c $< -o $@

build/csrc/sp_parse_lib.o: src/spinel_parse.c $(PRISM_LIB) | build/csrc
	$(CC) $(CFLAGS) -I$(PRISM_INC) -c src/spinel_parse.c -o $@

$(SPINEL): $(SPINEL_OBJ) build/csrc/sp_parse_lib.o $(PRISM_LIB)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(SPINEL_OBJ) build/csrc/sp_parse_lib.o $(PRISM_LIB) -lm $(LDFLAGS) -o $@
	@# Dev convenience: a repo-root `./spinel` pointing at the built binary
	@# (the installed command is `spinel` too). Best-effort; gitignored.
	@ln -sf $@ spinel 2>/dev/null || cp $@ spinel 2>/dev/null || true

# ---- Legacy Ruby compiler (regression oracle) ----
# Lives in legacy/ with its own Makefile; these are thin delegators.
legacy:
	+@$(MAKE) -C legacy --no-print-directory legacy

bootstrap:
	+@$(MAKE) -C legacy --no-print-directory bootstrap

codegen: legacy

# ---- RBS extractor ----
# Reads sig/**/*.rbs, emits the seed-file format spinel_analyze consumes
# when invoked with `spinel --rbs DIR`.

ifeq ($(wildcard $(RBS_INC)/rbs/parser.h),)
rbs_extract: rbs-missing
rbs-missing:
	@echo "Error: rbs C parser not found at $(RBS_INC)/rbs/parser.h."; \
	 echo "  Run 'make deps' to fetch it from rubygems.org into vendor/rbs."; \
	 exit 1
else
rbs_extract: $(RBS_EXTRACT_BIN)

$(RBS_EXTRACT_BIN): tools/spinel_rbs_extract.c $(RBS_LIB)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I$(RBS_INC) tools/spinel_rbs_extract.c $(RBS_LIB) -o $@
endif

# ---- Runtime library (regexp + bigint + …) ----

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

build/sp_pack.o: lib/sp_pack.c lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h
	@mkdir -p build
	$(CC) -c -O2 -Wno-all $(SEC_FLAGS) -Ilib lib/sp_pack.c -o build/sp_pack.o

build/sp_strscan.o: lib/sp_strscan.c lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h
	@mkdir -p build
	$(CC) -c -O2 -Wno-all $(SEC_FLAGS) -Ilib -Ilib/regexp lib/sp_strscan.c -o build/sp_strscan.o

build/sp_time.o: lib/sp_time.c lib/sp_time.h lib/sp_alloc.h
	@mkdir -p build
	$(CC) -c -O2 -Wno-all $(SEC_FLAGS) -Ilib lib/sp_time.c -o build/sp_time.o

build/sp_array.o: lib/sp_array.c lib/sp_array.h lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h
	@mkdir -p build
	$(CC) -c -O2 -Wno-all $(SEC_FLAGS) -Ilib lib/sp_array.c -o build/sp_array.o

build/sp_str.o: lib/sp_str.c lib/sp_str.h lib/sp_array.h lib/sp_alloc.h lib/sp_crypto.h
	@mkdir -p build
	$(CC) -c -O2 -Wno-all $(SEC_FLAGS) -Ilib lib/sp_str.c -o build/sp_str.o

build/sp_re.o: lib/sp_re.c lib/sp_re.h lib/sp_array.h lib/sp_str.h lib/sp_string.h lib/sp_inspect.h
	@mkdir -p build
	$(CC) -c -O2 -Wno-all $(SEC_FLAGS) -Ilib lib/sp_re.c -o build/sp_re.o

build/sp_core.o: lib/sp_core.c lib/sp_core.h
	@mkdir -p build
	$(CC) -c -O2 -Wno-all $(SEC_FLAGS) -Ilib lib/sp_core.c -o build/sp_core.o

build/sp_system.o: lib/sp_system.c lib/sp_system.h
	@mkdir -p build
	$(CC) -c -O2 -Wno-all $(SEC_FLAGS) -Ilib lib/sp_system.c -o build/sp_system.o

build/sp_gc.o: lib/sp_gc.c lib/sp_gc.h lib/sp_types.h
	@mkdir -p build
	$(CC) -c -O2 -Wno-all $(SEC_FLAGS) -Ilib lib/sp_gc.c -o build/sp_gc.o

build/sp_alloc.o: lib/sp_alloc.c lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h
	@mkdir -p build
	$(CC) -c -O2 -Wno-all $(SEC_FLAGS) -Ilib lib/sp_alloc.c -o build/sp_alloc.o

build/sp_json.o: lib/sp_json.c lib/sp_json.h lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h
	@mkdir -p build
	$(CC) -c -O2 -Wno-all $(SEC_FLAGS) -Ilib lib/sp_json.c -o build/sp_json.o

build/sp_format.o: lib/sp_format.c lib/sp_format.h lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h
	@mkdir -p build
	$(CC) -c -O2 -Wno-all $(SEC_FLAGS) -Ilib lib/sp_format.c -o build/sp_format.o

build/sp_stringio.o: lib/sp_stringio.c lib/sp_stringio.h lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h
	@mkdir -p build
	$(CC) -c -O2 -Wno-all $(SEC_FLAGS) -Ilib lib/sp_stringio.c -o build/sp_stringio.o

build/sp_string.o: lib/sp_string.c lib/sp_string.h lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h
	@mkdir -p build
	$(CC) -c -O2 -Wno-all $(SEC_FLAGS) -Ilib lib/sp_string.c -o build/sp_string.o

build/sp_inspect.o: lib/sp_inspect.c lib/sp_inspect.h lib/sp_string.h lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h
	@mkdir -p build
	$(CC) -c -O2 -Wno-all $(SEC_FLAGS) -Ilib lib/sp_inspect.c -o build/sp_inspect.o

build/sp_marshal.o: lib/sp_marshal.c lib/sp_marshal.h lib/sp_alloc.h lib/sp_gc.h lib/sp_types.h
	@mkdir -p build
	$(CC) -c -O2 -Wno-all $(SEC_FLAGS) -Ilib lib/sp_marshal.c -o build/sp_marshal.o

build/sp_fiber.o: lib/sp_fiber.c lib/sp_fiber.h lib/sp_gc.h lib/sp_types.h
	@mkdir -p build
	$(CC) -c -O2 -Wno-all $(SEC_FLAGS) -Ilib lib/sp_fiber.c -o build/sp_fiber.o

build/sp_net.o: lib/sp_net.c lib/sp_net.h
	@mkdir -p build
	$(CC) -c -O2 -Wno-all $(SEC_FLAGS) -Ilib lib/sp_net.c -o build/sp_net.o

build/sp_io.o: lib/sp_io.c lib/sp_io.h lib/sp_gc.h lib/sp_types.h
	@mkdir -p build
	$(CC) -c -O2 -Wno-all $(SEC_FLAGS) -Ilib lib/sp_io.c -o build/sp_io.o

SP_RT_LIB = lib/libspinel_rt.a

$(SP_RT_LIB): $(RE_OBJ) build/sp_bigint.o build/sp_crypto.o build/sp_pack.o build/sp_strscan.o build/sp_time.o build/sp_core.o build/sp_net.o build/sp_system.o build/sp_gc.o build/sp_alloc.o build/sp_json.o build/sp_marshal.o build/sp_format.o build/sp_stringio.o build/sp_string.o build/sp_inspect.o build/sp_array.o build/sp_str.o build/sp_re.o build/sp_fiber.o build/sp_io.o
	ar rcs $@ $^

regexp: $(SP_RT_LIB)

# ---- In-tree developer tools ----

# spinel-doctor / spinel-reduce / spinel-flatten: written in the spinel subset
# and compiled by spinel itself (dogfood), so their only runtime dependency is
# cc — the same as the compiler. Each tools/<name>.rb becomes bin/spinel-<name>,
# beside the compiler, so the `spinel-<name>` command is found next to `spinel`.
# A tool that no longer fits the subset breaks the build, which keeps them honest.
TOOL_NAMES = doctor reduce flatten
TOOL_BINS  = $(addprefix bin/spinel-,$(TOOL_NAMES))

tools: $(TOOL_BINS)

bin/spinel-%: tools/%.rb tools/tool_common.rb $(SPINEL) $(SP_RT_LIB)
	@mkdir -p bin
	$(SPINEL) $< -o $@

# ---- Test ----

TESTS := $(wildcard test/*.rb)
# Mode-incompatible: int_overflow_raises pins raise-mode semantics; under
# --int-overflow=promote the same code auto-promotes and output diverges.
ifeq ($(SPINEL_INT_OVERFLOW),promote)
TESTS := $(filter-out test/int_overflow_raises.rb,$(TESTS))
# Drive the spinel front-end and the C compile in promote mode so the test
# rule actually exercises the auto-promotion path end to end.
SP_OV_FLAG := --int-overflow=promote
SP_OV_DEFINE := -DSP_INT_OVERFLOW_MODE_PROMOTE
else
# `promote_*` tests overflow on purpose and only have defined output under
# --int-overflow=promote; in raise/wrap mode they would (correctly) raise.
TESTS := $(filter-out test/promote_%.rb,$(TESTS))
endif
TEST_TARGETS := $(patsubst test/%.rb,build/test-results/%.ok,$(TESTS))

# Warnings the generated-C -Werror check should not gate on. clang enables
# -Wunused-value by default (gcc only under -Wall, which the build disables),
# so a discarded value-producing statement-expression -- e.g. the
# `({ ...; v; })` emitted for `Fiber[:k] = v` in statement position -- fails
# CI under clang while gcc is silent. The value is intentionally discarded;
# behaviour is still gated by the output diff. Keep this list minimal.
TEST_WARN_SUPPRESS := -Wno-unused-value

# `make test` always runs fresh: it wipes the prior `.ok` stamps first,
# then runs the suite. (The old incremental `test` + `retest` split is
# gone — a stale `.ok` reading PASS was a recurring foot-gun.)
test:
	+@$(MAKE) --no-print-directory clean-test-results
	+@$(MAKE) --no-print-directory test-run

# The actual run. rbs-test golden-checks the RBS extractor (cheap, C-only).
# rbs-seed-test checks the seeds actually reach the analyzer (incl. nested
# classes, #1417). analyze-fail (legacy-analyzer diagnostics) lives in legacy/
# and runs only via an explicit `make analyze-fail-test` / `cd legacy && make`,
# not as part of `make gate` or the hot `make test` loop.
test-run: rbs-test rbs-seed-test $(TEST_TARGETS)
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

# ---- RBS extractor golden tests ----
RBS_TEST_SRCS := $(sort $(wildcard test/rbs/*.rbs))

ifeq ($(wildcard $(RBS_INC)/rbs/parser.h),)
rbs-test:
	@echo "rbs-test: skipped (vendor/rbs not fetched; run 'make deps')"
regen-rbs-expected:
	@echo "regen-rbs-expected: skipped (vendor/rbs not fetched; run 'make deps')"
else
rbs-test: $(RBS_EXTRACT_BIN)
	@fail=0; n=0; \
	for f in $(RBS_TEST_SRCS); do \
	  n=$$((n+1)); \
	  exp="$${f%.rbs}.seed.expected"; \
	  if [ ! -f "$$exp" ]; then echo "rbs-test: MISSING golden $$exp"; fail=1; continue; fi; \
	  d=$$($(RBS_EXTRACT_BIN) "$$f" 2>/dev/null | diff -u "$$exp" - 2>&1); \
	  if [ -z "$$d" ]; then \
	    if [ -t 1 ]; then printf .; fi; \
	  else \
	    echo; echo "rbs-test FAIL: $$f"; echo "$$d"; fail=1; \
	  fi; \
	done; \
	if [ -t 1 ]; then printf '\n'; fi; \
	if [ $$fail -ne 0 ]; then echo "RBS extractor tests: FAIL"; exit 1; fi; \
	echo "RBS extractor tests: $$n pass"

regen-rbs-expected: $(RBS_EXTRACT_BIN)
	@for f in $(RBS_TEST_SRCS); do \
	  $(RBS_EXTRACT_BIN) "$$f" > "$${f%.rbs}.seed.expected"; \
	  echo "regen: $${f%.rbs}.seed.expected"; \
	done
endif

# End-to-end --rbs seeding check (#1417). The extractor emits a module-nested
# class under its qualified name (`Outer_Box`), but the compiler's class table
# stores the leaf name (`Box`) + enclosing_class. seed_class_index must match
# the two so the seed reaches the class. The fixture's `@label` is declared
# `String?` but only ever assigned nil, so inference alone leaves it poly --
# only an applied seed pins it to a `const char *` field. The extractor must sit
# beside $(SPINEL) (main.c looks for it as a sibling), so copy it there first.
ifeq ($(wildcard $(RBS_INC)/rbs/parser.h),)
rbs-seed-test:
	@echo "rbs-seed-test: skipped (vendor/rbs not fetched; run 'make deps')"
else
rbs-seed-test: $(SPINEL) $(RBS_EXTRACT_BIN) $(SP_RT_LIB)
	@cp -f $(RBS_EXTRACT_BIN) $(dir $(SPINEL))spinel_rbs_extract
	@tmp=$$(mktemp -d /tmp/spinel-rbsseed.XXXXXX); ok=1; \
	$(SPINEL) test/rbs-seed/nested_ivar.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/out.c" 2>/dev/null; \
	grep -Eq 'const char[[:space:]]+\*[[:space:]]*iv_label' "$$tmp/out.c" || { echo "rbs-seed-test: FAIL (#1417: module-nested-class seed not applied)"; ok=0; }; \
	if grep -Eq 'sp_RbVal[[:space:]]+iv_label' "$$tmp/out.c"; then echo "rbs-seed-test: FAIL (#1417: ivar stayed poly)"; ok=0; fi; \
	$(CC) -fsyntax-only -Ilib "$$tmp/out.c" 2>/dev/null || { echo "rbs-seed-test: FAIL (nested_ivar C invalid)"; ok=0; }; \
	$(SPINEL) test/rbs-seed/boundary.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/b.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib "$$tmp/b.c" $(SP_RT_LIB) -lm -o "$$tmp/b" 2>"$$tmp/b.err"; then \
	  "$$tmp/b" > "$$tmp/b.out" 2>/dev/null; \
	  cmp -s "$$tmp/b.out" test/rbs-seed/boundary.expected || { echo "rbs-seed-test: FAIL (#1417 boundary output mismatch)"; diff -u test/rbs-seed/boundary.expected "$$tmp/b.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (#1417 boundary coercion C did not compile)"; ok=0; fi; \
	$(SPINEL) test/rbs-seed/void_block_tail.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/v.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib "$$tmp/v.c" $(SP_RT_LIB) -lm -o "$$tmp/v" 2>"$$tmp/v.err"; then \
	  "$$tmp/v" > "$$tmp/v.out" 2>/dev/null; \
	  cmp -s "$$tmp/v.out" test/rbs-seed/void_block_tail.expected || { echo "rbs-seed-test: FAIL (void block tail output mismatch)"; diff -u test/rbs-seed/void_block_tail.expected "$$tmp/v.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (void-returning call as proc tail: C did not compile)"; ok=0; fi; \
	$(SPINEL) test/rbs-seed/map_untyped_poly.rb --rbs test/rbs-seed/sig \
	  -c --no-line-map -o "$$tmp/mu.c" 2>/dev/null; \
	if $(CC) -O0 -Ilib "$$tmp/mu.c" $(SP_RT_LIB) -lm -o "$$tmp/mu" 2>"$$tmp/mu.err"; then \
	  "$$tmp/mu" > "$$tmp/mu.out" 2>/dev/null; \
	  cmp -s "$$tmp/mu.out" test/rbs-seed/map_untyped_poly.expected || { echo "rbs-seed-test: FAIL (untyped map-into-poly output mismatch)"; diff -u test/rbs-seed/map_untyped_poly.expected "$$tmp/mu.out" || true; ok=0; }; \
	else echo "rbs-seed-test: FAIL (untyped map result boxed as sp_box_int: C did not compile)"; ok=0; fi; \
	rm -rf "$$tmp"; \
	if [ $$ok -eq 1 ]; then echo "rbs-seed-test: pass"; else exit 1; fi
endif

# Analyze-fail fixtures test the legacy analyzer's rejection diagnostics;
# the recipe lives in legacy/Makefile. Thin delegator for `make gate`.
analyze-fail-test:
	+@$(MAKE) -C legacy --no-print-directory analyze-fail-test

# The .ok target is the test's stamp. Order-only $(SPINEL) keeps a
# compiler relink from invalidating every test.
build/test-results/%.ok: test/%.rb $(SP_RT_LIB) | $(SPINEL)
	@mkdir -p build/test-results
	@tmpdir=$$(mktemp -d /tmp/spinel-test.XXXXXX); \
	ast=$$tmpdir/test.ast; \
	ir=$$tmpdir/test.ir; \
	cfile=$$tmpdir/test.c; \
	bin=$$tmpdir/test_bin; \
	exp=$$tmpdir/expected; \
	act=$$tmpdir/actual; \
	experr=$$tmpdir/experr; \
	acterr=$$tmpdir/acterr; \
	args=""; \
	if [ -f "$<.args" ]; then args=$$(cat "$<.args"); fi; \
	rm -f "$@.diff"; \
	$(SPINEL) "$<" $(SP_OV_FLAG) -c --no-line-map -o "$$cfile" 2>/dev/null && \
	$(CC) $(CFLAGS) $(SP_OV_DEFINE) -Werror $(TEST_WARN_SUPPRESS) $(SEC_FLAGS) -Ilib "$$cfile" $(SP_RT_LIB) $(LDFLAGS) -lm $(GC_FLAGS) -o "$$bin" 2>/dev/null; \
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
	  $(TIMEOUT10) "$$bin" $$args >"$$act" 2>"$$acterr"; \
	  LC_ALL=C sed 's/\r$$//' "$$act" >"$$act.n"; \
	  LC_ALL=C sed 's/\r$$//' "$$acterr" >"$$acterr.n"; \
	  if [ -f "$<.err.expected" ]; then \
	    LC_ALL=C sed 's/\r$$//' "$<.err.expected" >"$$experr.n"; \
	  else \
	    : > "$$experr.n"; \
	  fi; \
	  if cmp -s "$$exp.n" "$$act.n" && cmp -s "$$experr.n" "$$acterr.n"; then \
	    echo PASS > "$@"; \
	    if [ -t 1 ]; then printf .; fi; \
	  else \
	    echo FAIL > "$@"; \
	    { echo "=== stdout diff (expected vs actual) ==="; diff -u "$$exp.n" "$$act.n" || true; \
	      echo "=== stderr diff (expected vs actual) ==="; diff -u "$$experr.n" "$$acterr.n" || true; } > "$@.diff" 2>&1; \
	    if [ -t 1 ]; then printf F; fi; \
	  fi; \
	else \
	  echo ERR > "$@"; \
	  if [ -t 1 ]; then printf E; fi; \
	fi; \
	rm -rf "$$tmpdir"

clean-test-results:
	@rm -rf build/test-results build/analyze-fail-results

# ---- Expected-output regeneration ----
# Snapshot each test's reference Ruby output so the test target uses the file
# directly and skips per-test ruby. .expected is stdout; .err.expected is stderr
# and is refreshed only where it already exists (a missing one means "stderr
# must be empty" and is left untouched).
EXPECTED_FILES     := $(patsubst test/%.rb,test/%.rb.expected,$(TESTS))
ERR_EXPECTED_FILES := $(wildcard test/*.rb.err.expected)

regen-expected: $(EXPECTED_FILES)
# Separate from regen-expected so a routine stdout refresh never rewrites the
# stderr sidecars (e.g. while a developer is using stderr for debugging).
regen-expected-err: $(ERR_EXPECTED_FILES)

# Regenerate $@ from the reference Ruby (falling back to a system ruby); $1 is
# the redirection selecting which stream to capture into $@.tmp. A failing
# oracle is skipped so a stale snapshot is kept rather than clobbered.
define regen-snapshot
@args=""; \
if [ -f "$<.args" ]; then args=$$(cat "$<.args"); fi; \
rc=0; $(TIMEOUT10) $(REF_RUBY) $< $$args $1 || rc=$$?; \
if [ $$rc -ne 0 ] && [ "$(REF_RUBY)" != "ruby" ]; then \
  rc=0; $(TIMEOUT10) ruby $< $$args $1 || rc=$$?; \
fi; \
if [ $$rc -ne 0 ]; then \
  echo "regen-expected: $< failed (rc=$$rc); skipping $@" >&2; rm -f $@.tmp; \
else \
  LC_ALL=C sed 's/\r$$//' $@.tmp > $@; rm -f $@.tmp; \
fi
endef

test/%.rb.expected: test/%.rb
	$(call regen-snapshot,>$@.tmp 2>/dev/null)

test/%.rb.err.expected: test/%.rb
	$(call regen-snapshot,2>$@.tmp >/dev/null)

bench: $(SPINEL) $(SP_RT_LIB)
	@if [ -z "$(TIMEOUT_BIN)" ]; then echo "Note: no 'timeout' command found; running without time limits."; fi
	@total=$$(ls benchmark/*.rb | wc -l); \
	if [ -t 1 ]; then tty=1; else tty=0; fi; \
	pass=0; fail=0; err=0; skip=0; i=0; \
	for f in benchmark/*.rb; do \
	  i=$$((i+1)); \
	  bn=$$(basename "$$f" .rb); \
	  if [ "$$tty" = 1 ]; then printf '\r\033[K  [%d/%d] %s' "$$i" "$$total" "$$bn"; fi; \
	  $(TIMEOUT10) $(SPINEL) "$$f" -c --no-line-map -o /tmp/_sp_b.c 2>/dev/null && \
	  $(CC) $(CFLAGS) -Werror $(TEST_WARN_SUPPRESS) $(SEC_FLAGS) -Ilib -c /tmp/_sp_b.c -o /tmp/_sp_b.c.o 2>/dev/null && \
	  $(CC) $(CFLAGS) /tmp/_sp_b.c.o $(SP_RT_LIB) $(LDFLAGS) -lm $(GC_FLAGS) -o /tmp/_sp_b_bin 2>/dev/null; \
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
	      $(TIMEOUT60) /tmp/_sp_b_bin >/tmp/_sp_b_act 2>/dev/null; \
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
	rm -f /tmp/_sp_b.ast /tmp/_sp_b.ir /tmp/_sp_b.c /tmp/_sp_b.c.o /tmp/_sp_b_bin /tmp/_sp_b_exp /tmp/_sp_b_act /tmp/_sp_b_exp.n /tmp/_sp_b_act.n; \
	echo "Benchmarks: $$pass pass, $$fail fail, $$err error, $$skip skip"; \
	if [ $$fail -ne 0 ] || [ $$err -ne 0 ]; then exit 1; fi

# ---- Optcarrot integration test ----
OPTCARROT_DIR  := build/optcarrot
OPTCARROT_REPO := https://github.com/mame/optcarrot.git
OPTCARROT_BRANCH := experiment/spinel

optcarrot: $(SPINEL) $(SP_RT_LIB)
	@if [ ! -d $(OPTCARROT_DIR) ]; then \
	  git clone --depth=1 --branch=$(OPTCARROT_BRANCH) $(OPTCARROT_REPO) $(OPTCARROT_DIR); \
	fi
	@ruby $(OPTCARROT_DIR)/tools/pack-for-spinel.rb > build/optcarrot-single.rb
	@$(SPINEL) build/optcarrot-single.rb -c --no-line-map -o build/optcarrot-single.c
	@$(CC) $(CFLAGS) -DSP_INT_OVERFLOW_MODE_WRAP -Ilib build/optcarrot-single.c $(SP_RT_LIB) $(LDFLAGS) -lm $(GC_FLAGS) -o build/optcarrot-single
	@out=$$($(TIMEOUT60) ./build/optcarrot-single 2>&1); \
	echo "$$out"; \
	if echo "$$out" | grep -qE "^fps: [0-9.]+$$" && echo "$$out" | grep -q "^checksum: 59662$$"; then \
	  echo "Optcarrot: OK"; \
	else \
	  echo "Optcarrot: FAIL — expected 'fps: <num>' and 'checksum: 59662'"; \
	  exit 1; \
	fi

# ---- Developer gates ----
#
# `test`, `bench` and `optcarrot` only READ the compiler binaries and
# write to disjoint build/ dirs, so they run concurrently as parallel
# prerequisites. Every recursive $(MAKE) is `+`-prefixed so the jobserver
# fd is inherited; none pass an explicit -j (which would force a sub-make
# to spawn its own pool → oversubscription).

# Fast pre-commit: rebuild the compiler and run the suite. OPT=-O1 compiles
# the sp_runtime.h-heavy per-test C ~3x faster than -O0 (the optimizer prunes
# the 800+ unreferenced static fns before codegen). Skips bench/optcarrot —
# run `make gate` before pushing for those.
check:
	+@$(MAKE) --no-print-directory all
	+@$(MAKE) --no-print-directory test OPT=-O1

# Full pre-push gate: test || bench || optcarrot in parallel. The C compiler is
# master; the legacy Ruby self-host bootstrap and its analyze-fail diagnostics
# are NOT part of the gate (run `make bootstrap` / `cd legacy && make` explicitly
# for those).
gate:
	+@$(MAKE) --no-print-directory all
	+@$(MAKE) --no-print-directory gate-legs
	@echo "gate: ALL GREEN"

gate-legs: gate-test gate-bench gate-optcarrot
gate-test:
	+@$(MAKE) --no-print-directory test OPT=-O1
gate-bench:
	+@$(MAKE) --no-print-directory bench
gate-optcarrot:
	+@$(MAKE) --no-print-directory optcarrot

# ---- Install ----

PREFIX   ?= /usr/local
SPNLDIR   = $(PREFIX)/lib/spinel

# Install the single `spinel` binary only. The legacy Ruby compiler is a
# headless regression oracle (legacy/, `make legacy`/`bootstrap`/
# `analyze-fail-test`) and is not shipped.
install: all
	install -d $(SPNLDIR)/lib
	install -m 755 $(SPINEL)            $(SPNLDIR)/spinel
	install -m 644 lib/libspinel_rt.a    $(SPNLDIR)/lib/
	install -m 644 lib/sp_runtime.h      $(SPNLDIR)/lib/
	install -m 644 lib/sp_types.h        $(SPNLDIR)/lib/
	install -m 644 lib/sp_core.h         $(SPNLDIR)/lib/
	install -m 644 lib/sp_system.h       $(SPNLDIR)/lib/
	install -m 644 lib/sp_gc.h           $(SPNLDIR)/lib/
	install -m 644 lib/sp_alloc.h        $(SPNLDIR)/lib/
	install -m 644 lib/sp_json.h         $(SPNLDIR)/lib/
	install -m 644 lib/sp_marshal.h      $(SPNLDIR)/lib/
	install -m 644 lib/sp_format.h       $(SPNLDIR)/lib/
	install -m 644 lib/sp_stringio.h     $(SPNLDIR)/lib/
	install -m 644 lib/sp_string.h       $(SPNLDIR)/lib/
	install -m 644 lib/sp_inspect.h      $(SPNLDIR)/lib/
	install -m 644 lib/sp_array.h        $(SPNLDIR)/lib/
	install -m 644 lib/sp_str.h          $(SPNLDIR)/lib/
	install -m 644 lib/sp_re.h           $(SPNLDIR)/lib/
	install -m 644 lib/sp_fiber.h         $(SPNLDIR)/lib/
	install -m 644 lib/sp_io.h           $(SPNLDIR)/lib/
	install -m 644 lib/sp_time.h         $(SPNLDIR)/lib/
	install -m 644 lib/sp_net.h          $(SPNLDIR)/lib/
	install -m 644 lib/*.rb              $(SPNLDIR)/lib/
	install -d $(PREFIX)/bin
	ln -sf $(SPNLDIR)/spinel $(PREFIX)/bin/spinel
	for t in $(TOOL_NAMES); do \
	  install -m 755 bin/spinel-$$t $(PREFIX)/bin/spinel-$$t; \
	done

uninstall:
	rm -f $(PREFIX)/bin/spinel
	for t in $(TOOL_NAMES); do rm -f $(PREFIX)/bin/spinel-$$t; done
	rm -rf $(SPNLDIR)

# ---- Clean ----

# Wipe the root build tree plus the legacy subtree's own build dir
# (legacy/build holds the legacy compiler's binaries + bootstrap
# intermediates). Only the root-level C artifacts need an explicit rm.
clean:
	rm -rf build/ bin/ legacy/build/
	rm -f legacy/spinel_parse spinel
