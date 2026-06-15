# Legacy Ruby compiler — bootstrap + self-host fixpoint oracle.
#
# This builds the self-hosted Ruby analyzer/codegen binaries
# (legacy/build/spinel_analyze / spinel_codegen) by interpreting the
# compiler on its own source through CRuby, then verifies the self-host
# fixpoint. The C compiler in ../src is the primary product; this tree is
# kept as a regression oracle and for IR-diff / .expected regeneration.
#
# Run from this directory:  make -C legacy bootstrap
# The parser and runtime library it needs are built in the parent tree;
# the parent Makefile's `bootstrap` / `legacy` targets delegate here.

ROOT  := ..
# Legacy artifacts live under legacy/build (self-contained in this subtree).
BUILD := build
include $(ROOT)/common.mk

.PHONY: all legacy bootstrap bootstrap-fixpoint codegen root-build \
        analyze-fail-test clean

# ---- Content stamps ----
# Downstream bootstrap rules depend on `$(BUILD)/stamps/foo.stamp` instead
# of `foo` directly, so `touch foo` (or a git checkout of identical content)
# doesn't invalidate the slow self-host bootstrap. The stamp advances only
# when content actually changes; FORCE re-checks cheaply each run. This
# machinery is legacy-only; the normal C build depends on sources directly.
$(BUILD)/stamps:
	@mkdir -p $@

FORCE:

$(BUILD)/stamps/%.stamp: FORCE | $(BUILD)/stamps
	@cmp -s $* $@ 2>/dev/null || cp $* $@

# Without .PRECIOUS make would delete these as pattern-rule intermediates,
# recreating them with fresh mtimes next run.
.PRECIOUS: $(BUILD)/stamps/%.stamp

# Default: the two CRuby-round compiler binaries.
all: legacy
legacy: $(BUILD)/spinel_analyze$(EXE) $(BUILD)/spinel_codegen$(EXE)
codegen: legacy

# Build the parser + runtime in the parent tree before we use them.
# Order-only: ensures the artifacts exist/are current without making
# their mtimes invalidate every downstream bootstrap intermediate.
root-build:
	+@$(MAKE) -C $(ROOT) --no-print-directory parse regexp

PARSE_BIN := $(ROOT)/spinel_parse$(EXE)
SP_RT_LIB := $(ROOT)/lib/libspinel_rt.a

ANALYZE_STAMP := $(BUILD)/stamps/spinel_analyze.rb.stamp
CODEGEN_STAMP := $(BUILD)/stamps/spinel_codegen.rb.stamp
LOADER_STAMP  := $(BUILD)/stamps/node_table_loader.rb.stamp
HELPERS_STAMP := $(BUILD)/stamps/compiler_helpers.rb.stamp
SRC_STAMPS    := $(ANALYZE_STAMP) $(CODEGEN_STAMP) $(LOADER_STAMP) $(HELPERS_STAMP)

# ---- Round 1: CRuby seeds the compiler from its own source ----
# Split into per-stage rules so parallel make can't read a stale AST
# (the side-effect-AST antipattern raced build/codegen2.ir before; #620).

$(BUILD)/analyze.ast: $(ANALYZE_STAMP) $(LOADER_STAMP) $(HELPERS_STAMP) | root-build
	$(PARSE_BIN) spinel_analyze.rb $@

$(BUILD)/codegen.ast: $(CODEGEN_STAMP) $(LOADER_STAMP) $(HELPERS_STAMP) | root-build
	$(PARSE_BIN) spinel_codegen.rb $@

$(BUILD)/analyze.ir: $(BUILD)/analyze.ast $(SRC_STAMPS)
	$(BOOTSTRAP_RUBY) spinel_analyze.rb $(BUILD)/analyze.ast $@

$(BUILD)/codegen.ir: $(BUILD)/codegen.ast $(SRC_STAMPS)
	$(BOOTSTRAP_RUBY) spinel_analyze.rb $(BUILD)/codegen.ast $@

$(BUILD)/analyze1.c: $(BUILD)/analyze.ast $(BUILD)/analyze.ir $(SRC_STAMPS)
	$(BOOTSTRAP_RUBY) spinel_codegen.rb $(BUILD)/analyze.ast $(BUILD)/analyze.ir $@

$(BUILD)/codegen1.c: $(BUILD)/codegen.ast $(BUILD)/codegen.ir $(SRC_STAMPS)
	$(BOOTSTRAP_RUBY) spinel_codegen.rb $(BUILD)/codegen.ast $(BUILD)/codegen.ir $@

$(BUILD)/spinel_analyze$(EXE): $(BUILD)/analyze1.c | root-build
	$(CC) $(BOOTSTRAP_CFLAGS) -I$(ROOT)/lib $(BUILD)/analyze1.c $(SP_RT_LIB) $(LDFLAGS) -lm -o $@

$(BUILD)/spinel_codegen$(EXE): $(BUILD)/codegen1.c | root-build
	$(CC) $(BOOTSTRAP_CFLAGS) -I$(ROOT)/lib $(BUILD)/codegen1.c $(SP_RT_LIB) $(LDFLAGS) -lm -o $@

# ---- Self-host fixpoint (round 2 == round 3) ----
# Round 2: the gen-1 binaries recompile each source into bin2_*; round 3:
# bin2_* recompile again and we assert byte equality of both IR and C.

$(BUILD)/analyze2.ir: $(BUILD)/analyze.ast $(BUILD)/spinel_analyze$(EXE)
	$(BUILD)/spinel_analyze$(EXE) $(BUILD)/analyze.ast $@
$(BUILD)/analyze2.c: $(BUILD)/analyze.ast $(BUILD)/analyze2.ir $(BUILD)/spinel_codegen$(EXE)
	$(BUILD)/spinel_codegen$(EXE) $(BUILD)/analyze.ast $(BUILD)/analyze2.ir $@
$(BUILD)/bin2_analyze$(EXE): $(BUILD)/analyze2.c
	$(CC) $(BOOTSTRAP_CFLAGS) -I$(ROOT)/lib $(BUILD)/analyze2.c $(SP_RT_LIB) $(LDFLAGS) -lm -o $@

$(BUILD)/codegen2.ir: $(BUILD)/codegen.ast $(BUILD)/spinel_analyze$(EXE)
	$(BUILD)/spinel_analyze$(EXE) $(BUILD)/codegen.ast $@
$(BUILD)/codegen2.c: $(BUILD)/codegen.ast $(BUILD)/codegen2.ir $(BUILD)/spinel_codegen$(EXE)
	$(BUILD)/spinel_codegen$(EXE) $(BUILD)/codegen.ast $(BUILD)/codegen2.ir $@
$(BUILD)/bin2_codegen$(EXE): $(BUILD)/codegen2.c
	$(CC) $(BOOTSTRAP_CFLAGS) -I$(ROOT)/lib $(BUILD)/codegen2.c $(SP_RT_LIB) $(LDFLAGS) -lm -o $@

$(BUILD)/analyze3.ir: $(BUILD)/analyze.ast $(BUILD)/bin2_analyze$(EXE)
	$(BUILD)/bin2_analyze$(EXE) $(BUILD)/analyze.ast $@
$(BUILD)/analyze3.c: $(BUILD)/analyze.ast $(BUILD)/analyze3.ir $(BUILD)/bin2_codegen$(EXE)
	$(BUILD)/bin2_codegen$(EXE) $(BUILD)/analyze.ast $(BUILD)/analyze3.ir $@
$(BUILD)/codegen3.ir: $(BUILD)/codegen.ast $(BUILD)/bin2_analyze$(EXE)
	$(BUILD)/bin2_analyze$(EXE) $(BUILD)/codegen.ast $@
$(BUILD)/codegen3.c: $(BUILD)/codegen.ast $(BUILD)/codegen3.ir $(BUILD)/bin2_codegen$(EXE)
	$(BUILD)/bin2_codegen$(EXE) $(BUILD)/codegen.ast $(BUILD)/codegen3.ir $@

# bootstrap: the CRuby-seeded self-host fixpoint AUTHORITY (CI/release).
# Verifies round2 == round3 byte-for-byte, then promotes bin2 to the
# legacy/build binaries.
bootstrap: bootstrap-fixpoint
bootstrap-fixpoint: $(BUILD)/analyze3.c $(BUILD)/codegen3.c
	@diff $(BUILD)/analyze2.ir $(BUILD)/analyze3.ir > /dev/null && echo "analyze.rb: IR fixpoint OK" || (echo "BOOTSTRAP FAILED: analyze.rb IR diverged" && exit 1)
	@diff $(BUILD)/analyze2.c  $(BUILD)/analyze3.c  > /dev/null && echo "analyze.rb: C fixpoint OK"  || (echo "BOOTSTRAP FAILED: analyze.rb C diverged"  && exit 1)
	@diff $(BUILD)/codegen2.ir $(BUILD)/codegen3.ir > /dev/null && echo "codegen.rb: IR fixpoint OK" || (echo "BOOTSTRAP FAILED: codegen.rb IR diverged" && exit 1)
	@diff $(BUILD)/codegen2.c  $(BUILD)/codegen3.c  > /dev/null && echo "codegen.rb: C fixpoint OK"  || (echo "BOOTSTRAP FAILED: codegen.rb C diverged"  && exit 1)
	cp $(BUILD)/bin2_analyze$(EXE) $(BUILD)/spinel_analyze$(EXE)
	cp $(BUILD)/bin2_codegen$(EXE) $(BUILD)/spinel_codegen$(EXE)

# ---- Analyze-fail fixtures ----
# Programs intentionally rejected during the analyze phase: they cover
# subset limits that must yield a stable diagnostic, not a binary. These
# exercise the legacy analyzer's diagnostics, so they live here.
ANALYZE_FAIL_TESTS := $(sort $(wildcard $(ROOT)/test/analyze_fail/*.rb))

analyze-fail-test: $(BUILD)/spinel_analyze$(EXE) | root-build
	@mkdir -p $(BUILD)/analyze-fail-results
	@fail=0; n=0; \
	for f in $(ANALYZE_FAIL_TESTS); do \
	  n=$$((n+1)); \
	  bn=$$(basename "$$f" .rb); \
	  tmpdir=$$(mktemp -d /tmp/spinel-analyze-fail.XXXXXX); \
	  ast=$$tmpdir/test.ast; ir=$$tmpdir/test.ir; err=$$tmpdir/analyze.err; \
	  exp="$${f%.rb}.stderr.expected"; \
	  if ! $(PARSE_BIN) "$$f" "$$ast" >/dev/null 2>"$$tmpdir/parse.err"; then \
	    echo "analyze-fail ERR: $$bn (parse failed)"; \
	    cat "$$tmpdir/parse.err"; \
	    fail=1; rm -rf "$$tmpdir"; continue; \
	  fi; \
	  if $(BUILD)/spinel_analyze$(EXE) "$$ast" "$$ir" >/dev/null 2>"$$err"; then \
	    echo "analyze-fail FAIL: $$bn (analyze succeeded)"; \
	    fail=1; rm -rf "$$tmpdir"; continue; \
	  fi; \
	  if [ ! -f "$$exp" ]; then \
	    echo "analyze-fail ERR: $$bn missing $$exp"; \
	    fail=1; rm -rf "$$tmpdir"; continue; \
	  fi; \
	  LC_ALL=C sed 's/\r$$//' "$$exp" > "$$tmpdir/expected.n"; \
	  LC_ALL=C sed 's/\r$$//' "$$err" > "$$tmpdir/actual.n"; \
	  if cmp -s "$$tmpdir/expected.n" "$$tmpdir/actual.n"; then \
	    echo PASS > "$(BUILD)/analyze-fail-results/$$bn.ok"; \
	  else \
	    echo "analyze-fail FAIL: $$bn"; \
	    diff -u "$$tmpdir/expected.n" "$$tmpdir/actual.n" || true; \
	    fail=1; \
	  fi; \
	  rm -rf "$$tmpdir"; \
	done; \
	echo "Analyze-fail tests: $$n pass"; \
	if [ $$fail -ne 0 ]; then exit 1; fi

# All legacy artifacts live under legacy/build; `make -C .. clean` wipes it
# along with the root build tree. This target just drops the binaries.
clean:
	rm -f $(BUILD)/spinel_analyze$(EXE) $(BUILD)/spinel_codegen$(EXE)
