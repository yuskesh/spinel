# Shared build configuration for Spinel.
#
# Included by both ./Makefile (the C compiler + test/bench harness) and
# legacy/Makefile (the legacy Ruby compiler bootstrap / self-host oracle).
# Keep only toolchain-level knobs here; target rules live in the two
# Makefiles that include this file.

# Machine-local overrides (gitignored). Lets a developer point the
# bootstrap at an alternative Ruby and set OPT/CC/etc. without editing
# committed defaults. A command-line `make VAR=...` still wins.
-include $(dir $(lastword $(MAKEFILE_LIST)))local.mk

CC       ?= cc
# Auto-wrap CC with sccache or ccache when present. Skip when CC is
# already wrapped — the substring guard catches both "ccache" and
# "sccache" since "ccache" is a substring of "sccache", so CI's
# `CC=sccache <cc>` env stays untouched. NO_CCACHE=1 opts out.
# `override` is required so that a command-line `make CC=gcc` is also
# wrapped — without it the wrap would be silently ignored.
ifeq (,$(findstring ccache,$(CC)))
ifeq (,$(NO_CCACHE))
  CCACHE_BIN := $(shell command -v sccache 2>/dev/null || command -v ccache 2>/dev/null)
  ifneq (,$(CCACHE_BIN))
    override CC := $(CCACHE_BIN) $(CC)
  endif
endif
endif

# Optimization level for the C compiles driven by the test/bench harness
# (each .rb gets parsed → codegen → cc'd → run). CI overrides this to -O0
# to cut cc time; locally -O2 keeps the test binaries reasonable-speed.
# `-Wno-alloc-size-larger-than` silences a gcc 13+ paranoia warning on the
# inlined `h->cap *= 2` in sp_*Hash_grow; `-Wno-unknown-warning-option`
# keeps clang (which lacks that flag) from turning it into a -Werror fail.
OPT     ?= -O2
CFLAGS   = $(OPT) -Wno-all -Wno-unknown-warning-option -Wno-alloc-size-larger-than

# Bootstrap-only flags: the legacy compiler runs on the developer's
# machine only, so -O3 -flto buys ~5-10% wall-clock without constraining
# users (whose generated C is built with plain CFLAGS). Override with
# LTO=0 on toolchains without LTO, or for a faster debug relink.
LTO     ?= 1
ifeq ($(LTO),1)
  BOOTSTRAP_CFLAGS = -O3 -flto=auto -Wno-all
else
  BOOTSTRAP_CFLAGS = $(CFLAGS)
endif

# Per-function sections let the linker strip unused bigint/regexp code.
SEC_FLAGS = -ffunction-sections -fdata-sections
# Apple ld64 spells dead-code stripping --dead_strip; GNU ld --gc-sections.
ifeq ($(shell uname -s),Darwin)
  GC_FLAGS = -Wl,-dead_strip
else
  GC_FLAGS = -Wl,--gc-sections
endif

# MinGW gcc appends .exe; reflect that so dependency tracking matches
# reality. Windows' default 1MB stack is too small for the deeply
# recursive bootstrap compile (~75k frames), so bump it at link time.
ifeq ($(OS),Windows_NT)
  EXE = .exe
  LDFLAGS += -Wl,--stack,67108864
endif

# `timeout` is GNU coreutils — present on Linux, named `gtimeout` on macOS
# (brew coreutils). Detect both; if neither is found run without limits.
TIMEOUT_BIN := $(shell command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null)
TIMEOUT10 := $(if $(TIMEOUT_BIN),$(TIMEOUT_BIN) 10,)
TIMEOUT60 := $(if $(TIMEOUT_BIN),$(TIMEOUT_BIN) 60,)

# Default to a parallel build at the logical CPU count, unless the
# invocation already asked for a job count. Applied ONLY at the top level
# (MAKELEVEL 0): a sub-make inherits the parent's jobserver, and forcing
# -j again in a sub-make's MAKEFLAGS triggers GNU Make's "-j forced in
# makefile: resetting jobserver mode" warning (the --jobserver-auth flag
# isn't visible in MAKEFLAGS at sub-make parse time, so a plain -j guard
# can't see it — MAKELEVEL is the reliable discriminator). A command-line
# -jN still wins by precedence; the env form `MAKEFLAGS=-jN` is honored
# by the inner guard.
ifeq ($(MAKELEVEL),0)
ifeq (,$(filter -j%,$(MAKEFLAGS))$(findstring j,$(firstword -$(MAKEFLAGS))))
  MAKEFLAGS += -j$(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
endif
endif

# Reference Ruby for test/bench output comparison. Override on the command
# line to use a freshly-built interpreter, e.g. `REF_RUBY=miniruby make test`.
REF_RUBY ?= ruby

# Ruby used to run the legacy bootstrap compiler. Long-running hot loops,
# so `YJIT=1 make` adds --yjit here (opt-in; needs a YJIT-enabled Ruby).
BOOTSTRAP_RUBY ?= ruby
ifeq ($(YJIT),1)
  BOOTSTRAP_RUBY := $(BOOTSTRAP_RUBY) --yjit
endif

# Content stamps are a legacy-bootstrap concern only; the rule lives in
# legacy/Makefile now. The normal C build depends on its sources directly.
