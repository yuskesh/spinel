# Spinel Optimization Plan

## Context

This document is written in the isolated worktree:

```text
/tmp/spinel-optcarrot-investigate
```

The main repository worktree must not be modified while this investigation is
in progress.

The optcarrot regression investigation in `PR908.md` reached three important
conclusions:

1. PR #908 fixed correct Ruby semantics for `&&` / `||` value returns.
2. A targeted logical-operator codegen fix was implemented and tested, but it
   made current HEAD slower.
3. The broader regression is layout- and IPC-sensitive, not a single obvious
   instruction-count regression.

The most useful new data point is that PGO dramatically improves optcarrot:

```text
normal -O2:   ~552-561 fps
PGO -O3:      ~827-858 fps
```

Representative counters:

```text
normal -O2:   ~4.59B instructions, ~2.10B cycles, IPC ~2.18
PGO -O3:      ~3.95B instructions, ~1.65B cycles, IPC ~2.39
```

PGO also reduced executed branch count:

```text
normal -O2:   ~1.00B branches
PGO -O3:      ~0.77B branches
```

This strongly suggests that generated C contains cold fallback/error/setup
paths mixed into hot code, and the C compiler needs either runtime profile
data or stronger static guidance from Spinel.

## Regression Anchors From PR908.md

The tail of `PR908.md` lists the important optcarrot slowdown anchors:

```text
d54cf0b  ~05-25  671 fps
1c02519  05-27   635 fps  (-36)
3afbdc4  05-27   615 fps  (-20)
4c68e4b  05-27   614 fps
234de48  05-27   576 fps  (-38, PR #908)
```

The broader anchor table is:

```text
5cf5c22  05-26 12:51  673 fps  3.675B instructions
b481894  05-26 22:16  651 fps  3.680B instructions
79b303d  05-27 end    571 fps  3.915B instructions
HEAD     06-02        562 fps  4.585B instructions
```

These anchors should guide the optimization work. They show at least two
mechanisms:

- instruction growth: `b481894 -> 79b303d` adds about `+235M` instructions;
- layout/IPC loss: some FPS drops occur with little instruction-count change.

The plan therefore must not focus only on PR #908 or only on logical operators.
It must address generated-code size, hot/cold separation, dynamic fallback
layout, and static approximations of PGO.

## Goals

- Recover optcarrot performance without reverting correct Ruby semantics.
- Identify which slowdown anchors correspond to instruction growth and which
  correspond to layout/IPC loss.
- Add static hot/cold separation where Spinel can know a path is rare.
- Keep PGO as an explicit high-performance mode.
- Build a repeatable measurement process so changes are not accepted based on
  noisy FPS alone.

## Non-Goals

- Do not make PGO the default. PGO runs the user program during compilation.
- Do not optimize only for optcarrot-specific names or classes.
- Do not remove required fallback/error behavior.
- Do not reapply the tested logical-op fix as-is; it regressed current HEAD.

## Measurement Protocol

Use checksum first:

```text
checksum: 59662
```

Use `perf stat` with a small event set:

```sh
perf stat -e instructions,cycles ./optcarrot_binary
perf stat -e branches,branch-misses,L1-icache-load-misses ./optcarrot_binary
```

Record for each experiment:

- commit or patch;
- generated C line count;
- binary `size`;
- FPS range across several runs;
- instructions;
- cycles;
- IPC;
- branch count;
- branch misses;
- L1 icache misses when available.

Primary decision metric:

- first correctness;
- then instructions and cycles;
- then FPS as a secondary user-visible signal.

## Phase 1: Reproduce Anchor Commits

Before changing codegen, rebuild and measure the PR908.md anchor commits in
isolated worktrees.

Targets:

- `d54cf0b`
- `1c02519`
- `3afbdc4`
- `4c68e4b`
- `234de48`
- `5cf5c22`
- `b481894`
- `79b303d`
- current HEAD

For each:

1. Generate optcarrot C from the same packed source when possible.
2. Compile with the same C flags.
3. Verify checksum.
4. Record counters.
5. Save generated C for diffing.

Expected output:

- a table separating instruction-count regressions from IPC/layout regressions;
- a generated-C diff summary for each large FPS cliff.

Reason:

The commit list tells us where performance fell. We need to know whether each
fall came from:

- more generated work;
- different C expression/statement shape;
- more fallback code in hot functions;
- binary layout changes;
- C compiler inlining/block-order changes.

## Phase 2: Keep PGO As Explicit Mode

PGO is currently the strongest known improvement.

Prototype interface:

```sh
spinel --pgo app.rb -o app -O 3
```

Behavior:

1. Generate C normally.
2. Compile with `-fprofile-generate`.
3. Run the instrumented binary once.
4. Recompile with `-fprofile-use -fprofile-correction`.

Required design points:

- `--pgo` must be explicit.
- It must print that it is executing the program for training.
- It needs a way to pass training arguments.
- It should work with `-E` naturally.
- It should support GCC first; Clang support can follow.

Possible argument form:

```sh
spinel --pgo app.rb -o app -- arg1 arg2
```

Risk:

- training run side effects;
- unrepresentative training data;
- compiler-specific flags.

Expected benefit:

- optcarrot reaches `~800+ fps` on the test machine.

## Phase 3: Add Hot/Cold Macros

Add portable generated-C macros:

```c
#if defined(__GNUC__) || defined(__clang__)
# define SP_LIKELY(x)   __builtin_expect(!!(x), 1)
# define SP_UNLIKELY(x) __builtin_expect(!!(x), 0)
# define SP_COLD        __attribute__((cold))
# define SP_HOT         __attribute__((hot))
# define SP_NOINLINE    __attribute__((noinline))
#else
# define SP_LIKELY(x)   (x)
# define SP_UNLIKELY(x) (x)
# define SP_COLD
# define SP_HOT
# define SP_NOINLINE
#endif
```

Initial rule:

- Use `SP_UNLIKELY` only when the path is semantically rare.
- Use `SP_COLD SP_NOINLINE` for helpers that only raise, report fallback, or
  handle impossible/uncommon dynamic cases.
- Do not add `SP_HOT` until later; it can easily distort layout.

## Phase 4: Extract Cold Raise/Error Paths

Spinel often emits raise paths inline. These should be separated first because
they are semantically cold.

Targets:

- `sp_raise_cls("NameError", ...)`
- `sp_raise_cls("NoMethodError", ...)`
- regexp compile error paths;
- integer overflow raise paths;
- type conversion failure paths;
- class initialization in-progress checks;
- wrong-class/wrong-shape dynamic fallback paths.

Before:

```c
if (bad_condition) {
  sp_raise_cls(...);
}
```

After:

```c
if (SP_UNLIKELY(bad_condition)) {
  sp_cold_raise_xxx(...);
}
```

with:

```c
static SP_COLD SP_NOINLINE void sp_cold_raise_xxx(...) {
  sp_raise_cls(...);
}
```

Expected benefit:

- smaller hot functions;
- better instruction locality;
- closer approximation to PGO `.cold` splitting.

## Phase 5: Extract Dynamic Dispatch Default Arms

Generated dynamic dispatch often has a hot set of known arms plus a rare
default. Defaults should be cold.

Targets:

- `switch (cls_id)` default arms;
- `sp_RbVal` tag dispatch defaults;
- poly receiver call fallback;
- typed array/poly array mixed fallbacks;
- typed hash/poly hash fallback arms;
- class constructor dispatch fallback;
- `Method` object dispatch fallback.

Pattern:

```c
switch (recv.cls_id) {
  case HOT_CLASS:
    return hot_call(...);
  default:
    return sp_cold_dispatch_missing(recv, ...);
}
```

Expected benefit:

- lower hot-path branch pressure;
- less fallback code in PPU/CPU hot loops;
- fewer layout cliffs when new features add cold fallback code.

## Phase 6: Analyze Feature-Wave Commits

`PR908.md` notes that surrounding 05-27 feature commits also contributed to the
drop:

- `1c02519`: string literal with embedded NUL preserves length;
- `3afbdc4`: str/poly hash inspect and sym/poly hash formatting;
- `4c68e4b`: array-container related merge;
- `234de48`: logical operator value semantics.

For each anchor:

1. Diff generated optcarrot C.
2. Identify new helpers and new fallback branches.
3. Determine whether they appear in hot PPU/CPU functions or only cold setup.
4. Try cold extraction for the new code.
5. Re-measure instructions/cycles.

Likely findings:

- Some commits add real work.
- Some commits only shift layout by adding cold code near hot code.
- Some commits change inlining decisions indirectly.

This phase should produce a ranked list of codegen areas that cause layout
damage.

## Phase 7: Function Ordering

PGO can reorder hot and cold functions. Spinel can approximate this.

Static hotness signals:

- method called inside a loop;
- method called by a method already marked hot;
- high direct-call fan-in;
- known direct call chain from program entry;
- methods containing generated loops or frequent dispatch.

Plan:

1. Collect a call graph during analysis/codegen.
2. Mark loop-contained call targets as hot.
3. Emit hot application methods first.
4. Emit ordinary methods next.
5. Emit cold helpers and fallback handlers last.

Risk:

- source order does not always equal binary order;
- `-ffunction-sections` and linker behavior may override source order.

Therefore test with:

- default `spinel` flags;
- without `--gc-sections`;
- with and without LTO if relevant.

## Phase 8: Loop-Aware Hotness Inference

Spinel can statically identify loops and propagate hotness.

Loop sources:

- `while`;
- `until`;
- range loops;
- `times`;
- `each`;
- known lowered iterator loops.

Use hotness for:

- inlining policy;
- cold-helper extraction aggressiveness;
- deciding whether dynamic fallback stays inline;
- function emission order;
- logical operator lowering shape.

Expected benefit:

- static approximation of PGO without running the program.

Risk:

- static hotness can be wrong in callback-heavy programs.

## Phase 9: Inlining Policy

The experiments showed that plain `-O3` is not always better than `-O2` with
current `spinel` flags. PGO makes inlining better because the compiler knows
hot paths.

Spinel should guide inlining more carefully.

Targets for inline:

- tiny accessors;
- simple predicates;
- arithmetic helpers;
- monomorphic wrappers.

Targets for noinline/cold:

- large fallback-heavy methods;
- raise helpers;
- dynamic dispatch defaults;
- setup-heavy methods outside hot loops.

Possible generated annotations:

```c
static inline ...
static SP_NOINLINE ...
static SP_COLD SP_NOINLINE ...
```

Acceptance rule:

- do not accept an inlining policy based on C size alone;
- require perf counters.

## Phase 10: Logical Operator Revisit

The tested condition-context logical-op fix regressed current HEAD. Keep this
phase after hot/cold work.

Targets:

- condition-context `AndNode` / `OrNode`;
- bool-only value fast path;
- avoiding `sp_RbVal` materialization in condition-only contexts.

Rules:

- preserve Ruby value semantics in expression contexts;
- preserve short-circuiting;
- do not assume inline C `&&` is faster;
- compare inline form, branchy temp form, and boxed form with perf counters.

Expected benefit:

- may reduce code size and work in non-optcarrot programs;
- may help after hot/cold layout changes make inline forms favorable again.

## Phase 11: Type-Inference Devirtualization

Many hot branches exist because values are widened to `poly`.

Targets:

- improve guard-based narrowing;
- improve ivar type stability;
- improve array/hash element propagation;
- keep small unions precise when possible;
- use RBS seeds without making them unsafely authoritative.

Expected benefit:

- fewer `sp_RbVal` boxes;
- fewer tag/cls_id dispatches;
- fewer fallback arms in generated C.

Risk:

- inference bugs can silently miscompile;
- needs broad tests.

## Phase 12: Runtime Helper Fast Paths

Some runtime helpers are generic and branch-heavy.

Targets:

- `sp_poly_truthy`;
- `sp_RbVal` tag checks;
- poly array access;
- typed array access through poly wrappers;
- typed hash missing-key logic;
- class ID recovery;
- object boxing helpers.

Approach:

- keep common tag/class arms inline;
- move uncommon arms into cold helpers;
- specialize helpers for statically small tag sets.

Expected benefit:

- lower dynamic dispatch cost;
- fewer branch instructions in hot loops.

## Phase 13: Compile Flag Policy

Current default compile flags include:

```text
-O2 -ffunction-sections -fdata-sections ... --gc-sections
```

Observed:

- `-O3` alone can help in one manual compile configuration;
- `spinel -O3` was worse than `spinel -O2` under current script flags;
- `-march=native` and `-Ofast` were not reliable improvements;
- PGO was clearly effective.

Policy:

- keep `-O2` default;
- keep `-O 3` available;
- add explicit `--pgo`;
- consider explicit experimental flags:

```sh
spinel --no-gc-sections app.rb
spinel --cc-opt=-fno-toplevel-reorder app.rb
```

Do not switch default to `-O3`.

## Phase 14: Benchmark Expansion

Do not optimize solely for optcarrot.

Benchmark categories:

- tight numeric loops;
- string-heavy code;
- regexp-heavy code;
- array/hash mixed access;
- poly dispatch;
- object allocation;
- fibers/procs/blocks;
- exception-heavy cold paths;
- template/web-like dynamic workloads.

For each benchmark, track:

- output correctness;
- generated C lines;
- binary size;
- instructions;
- cycles;
- IPC;
- branches;
- branch misses.

## Phase 15: Performance Report Target

Add a repeatable report target later:

```sh
make perf-report
```

It should emit a table, not enforce hard pass/fail on unstable machines.

Columns:

- benchmark;
- generated C lines;
- binary text size;
- instructions;
- cycles;
- IPC;
- branches;
- branch misses;
- FPS or wall time.

## Recommended Order

1. Reproduce and save data for the PR908.md root commits.
2. Add measurement scripts for optcarrot counters.
3. Add `SP_LIKELY`, `SP_UNLIKELY`, `SP_COLD`, `SP_NOINLINE`.
4. Extract raise/error paths into cold helpers.
5. Extract dynamic dispatch default arms into cold helpers.
6. Analyze the 05-27 feature-wave commits and rank codegen areas.
7. Add explicit `--pgo`.
8. Add call graph and loop-based hotness marks.
9. Use hotness for function ordering and inlining policy.
10. Revisit logical operator condition lowering.
11. Improve type inference and devirtualization.
12. Expand benchmark coverage.
13. Add `make perf-report`.

## Success Criteria

Short term:

- `--pgo` keeps optcarrot above `~800 fps` on the same machine class.
- static cold-path extraction improves normal non-PGO optcarrot.
- `make test` still passes.

Medium term:

- normal non-PGO optcarrot recovers a meaningful part of the PGO gain;
- branch count and cycles decrease in PPU/CPU hot loops;
- root-commit analysis explains which commits added work and which changed
  layout/IPC.

Long term:

- Spinel statically separates obvious cold paths.
- PGO remains available for safe training workloads.
- performance decisions are based on counters, not FPS alone.

## Appendix: Prototype `--pgo` Diff

This is the experimental diff from the isolated worktree
`/tmp/spinel-optcarrot-investigate`. It is included here as a reference only;
the main `spinel` script has not been changed by this document.

```diff
diff --git i/spinel w/spinel
index 34c4ff9..61c9e05 100755
--- i/spinel
+++ w/spinel
@@ -19,6 +19,8 @@
 #              of the command line becomes the binary's ARGV.
 #   -O LEVEL   Optimization level for cc (default: 2)
 #   --cc=CMD   C compiler command (default: cc)
+#   --pgo      Build with compiler PGO: compile instrumented binary,
+#              run it once, then recompile with the collected profile.
 #   -e STR     Inline Ruby source (repeatable; multiple -e join with \n)
 
 DIR="$(cd "$(dirname "$(readlink -f "$0" 2>/dev/null || realpath "$0" 2>/dev/null || echo "$0")")" && pwd)"
@@ -51,6 +53,8 @@ EXTRA_FLAGS=""
 C_TMP=""
 C_TMP_DIR=""
 RUN_TMP_DIR=""
+PGO_MODE=0
+PGO_TMP_DIR=""
 EVAL_SCRIPT=""
 EVAL_USED=0
 EVAL_TMP=""
@@ -69,6 +73,7 @@ while [ $# -gt 0 ]; do
     --source=*) SOURCE="${1#--source=}"; shift ;;
     --output=*) OUTPUT="${1#--output=}"; shift ;;
     --cc=*)     CC_CMD="${1#--cc=}"; shift ;;
+    --pgo)      PGO_MODE=1; shift ;;
     --rbs=*)    RBS_DIR="${1#--rbs=}"; shift ;;
     --rbs)      shift; RBS_DIR="$1"; shift ;;
     --int-overflow=*) INT_OVERFLOW="${1#--int-overflow=}"; shift ;;
@@ -177,6 +182,7 @@ if [ -z "$SOURCE" ]; then
   echo "  -E          Run the compiled binary; leftover args become its ARGV" >&2
   echo "  -O LEVEL    Optimization level (default: 2)" >&2
   echo "  --cc=CMD    C compiler (default: cc)" >&2
+  echo "  --pgo       Compile, run once for profile data, then recompile" >&2
   echo "  -e STR      Inline Ruby source (repeatable; joined with newlines)" >&2
   echo "  --rbs DIR   Seed analyzer with RBS signatures from DIR (advisory)" >&2
   echo "  --int-overflow=MODE  Int +/-/* overflow handling (default: raise)" >&2
@@ -198,7 +204,7 @@ else
 fi
 AST_TMP="$(mktemp /tmp/spinel_ast.XXXXXX)"
 IR_TMP="$(mktemp /tmp/spinel_ir.XXXXXX)"
-trap 'rm -f "$AST_TMP" "$IR_TMP" "$EVAL_TMP" "$SEED_TMP"; [ -n "$C_TMP_DIR" ] && rm -rf "$C_TMP_DIR"; [ -n "$RUN_TMP_DIR" ] && rm -rf "$RUN_TMP_DIR"' EXIT
+trap 'rm -f "$AST_TMP" "$IR_TMP" "$EVAL_TMP" "$SEED_TMP"; [ -n "$C_TMP_DIR" ] && rm -rf "$C_TMP_DIR"; [ -n "$RUN_TMP_DIR" ] && rm -rf "$RUN_TMP_DIR"; [ -n "$PGO_TMP_DIR" ] && rm -rf "$PGO_TMP_DIR"' EXIT
 
 # Step 1: Parse. spinel_parse is C-only; build it via `make parse`
 # (or `make all`) before invoking spinel.
@@ -326,10 +332,30 @@ case "$INT_OVERFLOW" in
   promote) EXTRA_FLAGS="$EXTRA_FLAGS -DSP_INT_OVERFLOW_MODE_PROMOTE" ;;
   *) echo "spinel: --int-overflow expects raise|wrap|promote, got '$INT_OVERFLOW'" >&2; exit 2 ;;
 esac
-$CC_CMD -O${OPT_LEVEL} -Wno-all -ffunction-sections -fdata-sections $INCLUDE_FLAGS $FFI_CFLAGS "$C_FILE" -lm $EXTRA_FLAGS $FFI_LINKS $GC_FLAG -o "$BIN_FILE"
-if [ $? -ne 0 ]; then
-  echo "spinel: C compilation failed" >&2
-  exit 1
+if [ "$PGO_MODE" -eq 1 ]; then
+  PGO_TMP_DIR="$(mktemp -d /tmp/spinel_pgo.XXXXXX)" || { echo "spinel: mktemp -d failed" >&2; exit 1; }
+  $CC_CMD -O${OPT_LEVEL} -Wno-all -ffunction-sections -fdata-sections -fprofile-generate="$PGO_TMP_DIR" $INCLUDE_FLAGS $FFI_CFLAGS "$C_FILE" -lm $EXTRA_FLAGS $FFI_LINKS $GC_FLAG -o "$BIN_FILE"
+  if [ $? -ne 0 ]; then
+    echo "spinel: PGO instrumented C compilation failed" >&2
+    exit 1
+  fi
+  echo "spinel: running PGO training binary" >&2
+  "$BIN_FILE" "$@"
+  if [ $? -ne 0 ]; then
+    echo "spinel: PGO training run failed" >&2
+    exit 1
+  fi
+  $CC_CMD -O${OPT_LEVEL} -Wno-all -ffunction-sections -fdata-sections -fprofile-use="$PGO_TMP_DIR" -fprofile-correction $INCLUDE_FLAGS $FFI_CFLAGS "$C_FILE" -lm $EXTRA_FLAGS $FFI_LINKS $GC_FLAG -o "$BIN_FILE"
+  if [ $? -ne 0 ]; then
+    echo "spinel: PGO optimized C compilation failed" >&2
+    exit 1
+  fi
+else
+  $CC_CMD -O${OPT_LEVEL} -Wno-all -ffunction-sections -fdata-sections $INCLUDE_FLAGS $FFI_CFLAGS "$C_FILE" -lm $EXTRA_FLAGS $FFI_LINKS $GC_FLAG -o "$BIN_FILE"
+  if [ $? -ne 0 ]; then
+    echo "spinel: C compilation failed" >&2
+    exit 1
+  fi
 fi
 
 if [ "$RUN_MODE" -eq 1 ]; then
```
