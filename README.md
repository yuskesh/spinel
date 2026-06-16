# Spinel -- Ruby AOT Compiler

Spinel compiles Ruby source code into standalone native executables.
It performs whole-program type inference and generates optimized C code,
achieving significant speedups over CRuby.

The compiler is a **single self-contained C binary**: it parses Ruby (via
libprism), infers types across the whole program, emits C, and invokes the
system `cc` to produce a native executable -- no Ruby runtime and no chained
helper binaries at compile time. (Spinel was previously written in a
self-hosting Ruby subset; that backend is preserved in-tree as a regression
oracle -- see [History](#history).)

## How It Works

```
Ruby (.rb)
    |
    v
parse (libprism)       Parse with Prism, linked in as a C library
    |                  (a CRuby + Prism-gem path produces the same AST)
    v
text AST -> NodeTable  Loaded into an in-memory node table
    |
    v
analyze                Whole-program type inference (src/analyze*.c).
    |                  Walks the AST to a fixpoint: param / return / ivar
    |                  types, value-type detection, dead-code markers,
    |                  a per-node inferred-type cache.
    v
codegen                C code generation (src/codegen*.c). Reads the AST
    |                  plus the analysis just computed in the same process,
    |                  emits one C file.
    v
C source (.c)
    |
    v
cc -O2 -Ilib -lm      System C compiler + runtime
    |
    v
Native binary           Standalone, no runtime dependencies
```

Analyze and codegen run in one process and share the in-memory model:
codegen reads the types analyze just inferred directly, with no
serialization step in between. See [docs/AST.md](docs/AST.md) for the
text AST format the parser emits and the analyzer consumes.

## Quick Start

```bash
# Fetch libprism sources (from the prism gem on rubygems.org):
make deps

# Build everything:
make

# Write a Ruby program:
cat > hello.rb <<'RUBY'
def fib(n)
  if n < 2
    n
  else
    fib(n - 1) + fib(n - 2)
  end
end

puts fib(34)
RUBY

# Compile and run:
./spinel hello.rb
./hello               # prints 5702887 (instantly)
```

### Options

```bash
./spinel app.rb              # compiles to ./app
./spinel app.rb -o myapp     # compiles to ./myapp
./spinel app.rb -c           # generates app.c only
./spinel app.rb -S           # prints C to stdout
./spinel -E app.rb a b c     # compile, run with ARGV=[a, b, c], discard binary
./spinel -e 'puts 42'        # compile inline source
./spinel app.rb --int-overflow=wrap   # +/-/* wrap silently instead of raising
```

`./spinel` is the compiler: a single native binary (`build/spinel`; the
repo-root `spinel` is a convenience symlink `make` creates) that parses,
infers types, emits C, invokes `cc` to link it, and can run the result —
no shell wrapper or chained helper binaries, so it works on Windows
natively. It supports the full option set, including `--rbs DIR`
(RBS-seeded inference) and the `--emit-rbs` / `--emit-types` /
`--emit-symbol-map` analysis modes. The legacy Ruby backend is kept only as
a headless regression oracle (the self-host fixpoint and analyze-fail
gates), driven by `make bootstrap` / `make analyze-fail-test`; it no longer
ships a user-facing driver.

#### Integer overflow

Integers are native fixed-width words. `--int-overflow=MODE` selects how
`+`/`-`/`*` behave when a result exceeds that width:

- `raise` (default) -- raise on overflow. Safe (never silently wrong or
  undefined), but `9223372036854775807 + 1` raises instead of returning a
  Bignum.
- `wrap` -- silent two's-complement wrap. Fastest, no check.
- `promote` -- escalate to arbitrary-precision `Integer` (Bigint), matching
  CRuby. `9223372036854775807 + 1` returns `9223372036854775808`.

In the default mode, integer locals that an obvious growth pattern would
overflow (e.g. a `q = q * k` accumulator) are still auto-promoted to Bigint;
`promote` extends that to every integer operation.

### RBS type signatures

Spinel can read RBS files to seed the analyzer. When invoked with
`--rbs DIR`, `spinel` runs `spinel_rbs_extract` over a directory of
`*.rbs` files (the same layout `rbs` and Steep use) and feeds the
resulting seed into the analyzer. Seeds are advisory — inference still runs on top and
widens on observed contradiction, so a wrong or unrepresentable seed
is at worst a no-op. See [docs/RBS-EXTRACT.md](docs/RBS-EXTRACT.md)
for the supported subset.

## Self-Hosting (legacy oracle)

Spinel began as a compiler written in a Ruby subset that compiled
itself. The C compiler has since replaced that backend, but the
self-hosting Ruby implementation is kept in-tree (under `legacy/`) as a
**regression oracle**. `make bootstrap` exercises its 4-way self-host
fixpoint -- the Ruby backend, compiled by the previous generation,
must reproduce byte-identical IR and C across two generations of both
the analyze and codegen sides:

```
analyze2.ir == analyze3.ir    (analyze: IR fixpoint OK)
analyze2.c  == analyze3.c     (analyze: C  fixpoint OK)
codegen2.ir == codegen3.ir    (codegen: IR fixpoint OK)
codegen2.c  == codegen3.c     (codegen: C  fixpoint OK)
```

Any change that affects deterministic output -- record order,
default-value handling, hash iteration -- breaks one of these checks.
The bootstrap is no longer part of the standard gate (the C build is
checked by `make test` / `make bench` / `make optcarrot`); it remains
available for verifying the legacy backend still round-trips.

## Benchmarks

886 tests pass. 57 benchmarks pass.
Geometric mean: **~7.8x faster** than Ruby 4.0.4 with `--yjit` across
the 28 benchmarks below. Baseline is CRuby 4.0.4 (stable), run with
`--disable-gems` and with `--yjit` for the JIT column. Each timing is
the best of three wall-clock runs; sub-10 ms cells are dominated by
interpreter / runtime startup and should be read as "noise floor."

### Computation

| Benchmark | Spinel | Ruby 4.0.4 | + YJIT | Speedup vs YJIT |
|---|---|---|---|---|
| mandelbrot | 24 ms | 1_339 ms | 1_340 ms | 55.8x |
| nqueens | 9 ms | 312 ms | 311 ms | 34.6x |
| matmul | 9 ms | 302 ms | 303 ms | 33.7x |
| life (Conway's GoL) | 23 ms | 861 ms | 497 ms | 21.6x |
| partial_sums | 80 ms | 1_324 ms | 1_310 ms | 16.4x |
| sieve | 25 ms | 406 ms | 409 ms | 16.4x |
| sudoku | 6 ms | 103 ms | 54 ms | 9.0x |
| fannkuch | 2 ms | 15 ms | 15 ms | 7.5x |
| fasta (DNA seq gen) | 2 ms | 14 ms | 14 ms | 7.0x |
| ackermann | 9 ms | 343 ms | 60 ms | 6.7x |
| fib (recursive) | 15 ms | 538 ms | 95 ms | 6.3x |
| tarai | 28 ms | 400 ms | 70 ms | 2.5x |
| tak | 34 ms | 502 ms | 82 ms | 2.4x |

### Data Structures & GC

| Benchmark | Spinel | Ruby 4.0.4 | + YJIT | Speedup vs YJIT |
|---|---|---|---|---|
| huffman (encoding) | 7 ms | 63 ms | 64 ms | 9.1x |
| so_lists | 35 ms | 485 ms | 299 ms | 8.5x |
| rbtree (red-black tree) | 20 ms | 521 ms | 113 ms | 5.6x |
| splay tree | 14 ms | 179 ms | 65 ms | 4.6x |
| binary_trees | 5 ms | 36 ms | 19 ms | 3.8x |
| linked_list | 71 ms | 306 ms | 236 ms | 3.3x |
| gcbench | 553 ms | 3_455 ms | 1_783 ms | 3.2x |

### Real-World Programs

| Benchmark | Spinel | Ruby 4.0.4 | + YJIT | Speedup vs YJIT |
|---|---|---|---|---|
| ao_render (ray tracer) | 103 ms | 3_033 ms | 1_122 ms | 10.9x |
| str_concat | 1 ms | 10 ms | 10 ms | 10.0x |
| bigint_fib (1000 digits) | 1 ms | 10 ms | 10 ms | 10.0x |
| json_parse | 41 ms | 413 ms | 275 ms | 6.7x |
| pidigits (bigint) | 2 ms | 10 ms | 10 ms | 5.0x |
| template engine | 170 ms | 1_020 ms | 723 ms | 4.3x |
| io_wordcount | 27 ms | 107 ms | 100 ms | 3.7x |
| csv_process | 255 ms | 1_050 ms | 924 ms | 3.6x |

A few notes on what YJIT does and doesn't change. On some integer-loop
workloads (mandelbrot, nqueens, matmul, partial_sums, sieve) YJIT's
numbers are essentially identical to interpreted Ruby; the benchmark
is bound by integer / float operations that the interpreter already
runs at native speed. On call-heavy code (ackermann, fib, tarai, tak,
rbtree) YJIT gives a real 4-6x lift, but Spinel still wins by ahead-
of-time specialization. The two YJIT-only weak spots remaining are
`tarai` and `tak`, where YJIT inlines the recursive call site so well
that Spinel's compiled C only beats it by ~2.4x.

## Supported Ruby Features

**Core**: Classes, inheritance, `super`, `include` (mixin), `attr_accessor`,
`Struct.new`, `alias`, module constants, open classes for built-in types.

**Control Flow**: `if`/`elsif`/`else`, `unless`, `case`/`when`,
`case`/`in` (pattern matching), `while`, `until`, `loop`, `for..in`
(range and array), `break`, `next`, `return`, `catch`/`throw`,
`&.` (safe navigation).

**Blocks**: `yield`, `block_given?`, `&block`, `proc {}`, `Proc.new`,
lambda `-> x { }`, `method(:name)`. Block methods: `each`,
`each_with_index`, `map`, `select`, `reject`, `reduce`, `sort_by`,
`any?`, `all?`, `none?`, `times`, `upto`, `downto`.

**Exceptions**: `begin`/`rescue`/`ensure`/`retry`, `raise`,
custom exception classes.

**Types**: Integer, Float, String (immutable + mutable), Array, Hash,
Range, Time, StringIO, File, Regexp, Bigint (auto-promoted), Fiber.
Polymorphic values via tagged unions. Nullable object types (`T?`)
for self-referential data structures (linked lists, trees).

**Inspect / `p`**: `Object#inspect` is implemented for all primitive
types (Integer, Float, String, Symbol, Boolean, nil), for typed
arrays (`int_array`, `float_array`, `str_array`, `sym_array`), and
for heterogeneous arrays (`poly_array`, e.g. `[1, "x", :y]`).
Scalar polymorphic values (the tagged-union values from the `Types`
section above) also inspect correctly.
`Array#to_s` is aliased to `Array#inspect`, matching CRuby.
`Kernel#p` dispatches through `compile_inspect_for` so `p obj`,
`obj.inspect`, `obj.to_s`, and `"#{obj.inspect}"` interpolation
all produce CRuby-byte-identical output. User-class instances
inside a polymorphic value currently render as the placeholder
`"#<Object>"` (the runtime has no class-name table yet); Hash,
Range, and Struct inspect are not yet implemented.

**Global Variables**: `$name` compiled to static C variables with
type-mismatch detection at compile time.

**Strings**: `<<` automatically promotes to mutable strings (`sp_String`)
for O(n) in-place append. `+`, interpolation, `tr`, `ljust`/`rjust`/`center`,
and all standard methods work on both. Character comparisons like
`s[i] == "c"` are optimized to direct char array access (zero allocation).
Chained concatenation (`a + b + c + d`) collapses to a single malloc
via `sp_str_concat4` / `sp_str_concat_arr` -- N-1 fewer allocations.
Loop-local `str.split(sep)` reuses the same `sp_StrArray` across
iterations (csv_process: 4 M allocations eliminated).

**Regexp**: Built-in NFA regexp engine (no external dependency).
`=~`, `$1`-`$9`, `match?`, `gsub(/re/, str)`, `sub(/re/, str)`,
`scan(/re/)`, `split(/re/)`.

**Bigint**: Arbitrary precision integers via mruby-bigint. Auto-promoted
from loop multiplication patterns (e.g. `q = q * k`), or from every integer
operation under `--int-overflow=promote` (see [Integer overflow](#integer-overflow)).
Linked as static library -- only included when used.

**Fiber**: Cooperative concurrency via `ucontext_t`. `Fiber.new`,
`Fiber#resume`, `Fiber.yield` with value passing. Captures free
variables via heap-promoted cells. Per-fiber storage via `Fiber[:k]`
/ `Fiber[:k] = v` (and the `Fiber.current[:k]` aliases) — symbol-keyed
poly-valued, lazily allocated, shallow-snapshot inherited from the
parent at `Fiber.new` time.

**Memory**: Mark-and-sweep GC with size-segregated free lists, non-recursive
marking, and sticky mark bits. Small classes (≤8 scalar fields, no
inheritance, no mutation through parameters) are automatically
stack-allocated as **value types** -- 1M allocations of a 5-field class
drop from 85 ms to 2 ms. Programs using only value types emit no GC
runtime at all.

**Symbols**: Separate `sp_sym` type, distinct from strings (`:a != "a"`).
Symbol literals are interned at compile time (`SPS_name` constants);
`String#to_sym` uses a dynamic pool only when needed. Symbol-keyed
hashes (`{a: 1}`) use a dedicated `sp_SymIntHash` that stores
`sp_sym` (integer) keys directly rather than strings -- no strcmp, no
dynamic string allocation.

**I/O**: `puts`, `print`, `printf`, `p`, `gets`, `ARGV`, `ENV[]`,
`File.read/write/open` (with blocks), `system()`, backtick.

**FFI**: Direct C calls without an extension compiler. Declarations
(`ffi_func`, `ffi_lib`, `ffi_const`, `ffi_buffer`, `ffi_read_*`) live
inside a `module` body; the codegen emits externs and the `spinel`
wrapper picks up `-l` flags from marker comments. Scalars, strings,
opaque `:ptr`, integer constants, raw byte buffers, and struct-field
reads are covered. See [docs/FFI.md](docs/FFI.md) for the full spec
and [examples/ffi/](examples/ffi/) for runnable demos against libc/
libm and sqlite3.

## Optimizations

Whole-program type inference drives several compile-time optimizations:

- **Value-type promotion**: small immutable classes (≤8 scalar fields)
  become C structs on the stack, eliminating GC overhead entirely.
- **Constant propagation**: simple literal constants (`N = 100`) are
  inlined at use sites instead of going through `cst_N` runtime lookup.
- **Loop-invariant length hoisting**: `while i < arr.length` evaluates
  `arr.length` once before the loop; `while i < str.length` hoists
  `strlen`. Mutation of the receiver inside the body (e.g. `arr.push`)
  correctly disables the hoist.
- **Method inlining**: short methods (≤3 statements, non-recursive)
  get `static inline` so gcc can inline them at call sites.
- **String concat chain flattening**: `a + b + c + d` compiles to a
  single `sp_str_concat4` / `sp_str_concat_arr` call -- one malloc
  instead of N-1 intermediate strings.
- **Bigint auto-promotion**: loops with `x = x * y` or fibonacci-style
  `c = a + b` self-referential addition auto-promote to bigint.
- **Bigint `to_s`**: divide-and-conquer O(n log²n) via mruby-bigint's
  `mpz_get_str` instead of naive O(n²).
- **Static symbol interning**: `"literal".to_sym` resolves to a
  compile-time `SPS_<name>` constant; the runtime dynamic pool is
  only emitted when dynamic interning is actually used.
- **`strlen` caching in sub_range**: when a string's length is
  hoisted, `str[i]` accesses use `sp_str_sub_range_len` to skip the
  internal strlen call.
- **split reuse**: `fields = line.split(",")` inside a loop reuses
  the existing `sp_StrArray` rather than allocating a new one.
- **Dead-code elimination**: compiled with `-ffunction-sections
  -fdata-sections` and linked with `--gc-sections`; each unused
  runtime function is stripped from the final binary.
- **Iterative inference early exit**: the param/return/ivar fixed-point
  loop stops as soon as a signature over the refined tables stops
  changing. Most programs converge in a couple of iterations, keeping
  compile time low.
- **Warning-free build**: generated C compiles cleanly at the default
  warning level across every test and benchmark; the harness uses
  `-Werror` so regressions surface immediately.

## Architecture

```
spinel                Single binary: parse + analyze + codegen + cc driver
                      (repo-root symlink to build/spinel)
src/spinel_parse.c    Frontend: libprism → text AST
src/node_table.c      AST loader: text AST → in-memory NodeTable
src/analyze*.c        Whole-program type inference
src/codegen*.c        C code generation
src/main.c            CLI driver: pipeline + cc invocation
lib/sp_runtime.h      Runtime library header (GC, arrays, hashes, strings)
lib/sp_*.c            Out-of-line runtime (bigint, GC, fiber, I/O, time, ...)
lib/regexp/           Built-in regexp engine; all linked into libspinel_rt.a
legacy/               Former self-hosting Ruby backend (regression oracle)
test/                 886 feature tests
benchmark/            57 benchmarks
docs/                 Format specs (AST, FFI, sp_Class design)
Makefile              Build automation
```

The analyzer and code generator are C (`src/analyze*.c`,
`src/codegen*.c`) sharing a common `Compiler` over the in-memory
`NodeTable`. The pipeline accepts the Ruby subset Spinel targets:
classes, `def`, `attr_accessor`, `if`/`case`/`while`,
`each`/`map`/`select`, `yield`, blocks (including `...` argument
forwarding), `begin`/`rescue`, String/Array/Hash operations, File I/O.

No dynamic metaprogramming or `eval`. Compile-time class-body
declarations with compile-time-known literal inputs are supported for
Struct-style method synthesis.

### What analyze does

The analyze stage (`analyze_program`) owns whole-program type
inference. It's a sequence of passes over the AST, each one filling in
or refining one piece of the static model:

1. **Registration** -- a walk that registers every class, module,
   top-level method, instance/class method, ivar, FFI declaration,
   regexp literal, and constant, then resolves parents, includes,
   prepends, attr/alias declarations and inherited members. After
   this the compiler's tables carry every name the program defines.

2. **Call-site widening** -- each scope's call sites feed their arg
   types into the callee's param slots. The unifier widens to `poly`
   only when two call sites disagree -- the conservative direction.

3. **Iterative refinement loop** -- return types, ivar types from
   writers, param types (array / hash / string specializations),
   block-param types, default-param types, and `for`/bigint loop
   locals are re-inferred to a fixpoint. The loop terminates when a
   signature over those tables stops changing; most programs converge
   in a couple of rounds.

4. **Feature detection** -- value-type detection (which small
   immutable classes become stack structs), GC need, symbol
   collection, proc-capture marking, and reachability. These set the
   flags that gate runtime-helper emission and mark classes / methods
   for dead-code elimination.

5. **Node-type annotation** -- a final pass fills a per-node
   inferred-type cache that codegen reads directly while emitting,
   avoiding any re-inference at emit time.

### What codegen does

The codegen stage runs in the same process on the same `Compiler`,
then emits one C file:

- It emits the preamble (`#include "sp_runtime.h"`, the per-program
  runtime helpers gated on the analysis flags, the sp_Class tables for
  hierarchy-using programs, the symbol intern table, class structs and
  constructors, forward declarations), walks every reachable method /
  class-method body to emit its definition, then emits `int main()`.

- Per-program runtime helpers are gated. A `puts "hi"` program emits a
  handful of lines of C; a program that touches Method instances, hash
  literals, the class hierarchy, etc. gets the matching runtime blocks.

The runtime is split between a header (`lib/sp_runtime.h`: GC,
array/hash/string implementations, inline hot paths) and out-of-line
sources (`lib/sp_*.c`, `lib/regexp/`) archived into `libspinel_rt.a`.
Generated C includes the header and links the archive; `--gc-sections`
drops every unused runtime function from the final binary.

The parser has two implementations:
- **src/spinel_parse.c** links libprism directly (no CRuby needed)
- **spinel_parse.rb** uses the Prism gem (CRuby fallback)

Both produce identical AST output; the C path is the default.
`require_relative` is resolved at parse time by inlining the
referenced file.

## Building

```bash
make deps         # fetch libprism into vendor/prism (one-time)
make              # build the C compiler (parser + regexp library + spinel)
make test         # run the feature tests (always a fresh run)
make bench        # run benchmarks vs CRuby
make legacy       # build the legacy Ruby compiler into legacy/build/
make bootstrap    # legacy self-host fixpoint check (4-way), in legacy/
sudo make install # install the C compiler to /usr/local (spinel in PATH)
make clean        # remove build artifacts
```

The legacy Ruby backend builds entirely under `legacy/build/` (binaries
and bootstrap intermediates) and is never installed — it is a headless
regression oracle for the self-host fixpoint (`make bootstrap`) and the
analyze-fail diagnostics (`make analyze-fail-test`). The normal C build
never touches the `legacy/` source tree.

Override install prefix: `make install PREFIX=$HOME/.local`

[Prism](https://github.com/ruby/prism) is the Ruby parser used by
`spinel_parse`. `make deps` downloads the prism gem tarball from
rubygems.org and extracts its C sources to `vendor/prism`. If you
already have the prism gem installed, the build auto-detects it; you
can also point at a custom location with `PRISM_DIR=/path/to/prism`.

CRuby is not needed to build or run the C compiler -- only as an
optional parser fallback (the Prism-gem path), to run the legacy
self-host oracle, and in the test harness, which compares Spinel's
output against CRuby on the same source.

## Portability

Spinel can emit C without invoking the C compiler — useful when you
want to build the Ruby program on one machine and ship the generated
sources to another:

```sh
spinel app.rb -c            # writes app.c next to the source
spinel app.rb -c -o app.c   # specify output path
spinel app.rb -S            # print the C to stdout
```

The output is one self-contained `.c` file. A downstream consumer
compiles it against the `lib/` headers (`sp_runtime.h` and friends) and
links `libspinel_rt.a` (the out-of-line runtime: bigint, regexp, GC,
fiber, I/O); `--gc-sections` then drops whatever the program doesn't
use. No CRuby and no `spinel` binary are needed on the target machine.

The runtime is POSIX-flavoured but covers every platform CI exercises:

| Platform | Status | Compiler |
|---|---|---|
| Linux (x86-64, arm64) | Supported | gcc, clang |
| macOS (Intel, Apple Silicon) | Supported | clang |
| *BSD | Expected to work; not in CI | clang |
| Windows | Supported via [MSYS2](https://www.msys2.org/) / MinGW | gcc |
| Windows native (MSVC) | Not supported | -- |

Every PR runs `ubuntu-latest / gcc`, `ubuntu-latest / clang`,
`macos-latest / clang`, and `windows-mingw` jobs end-to-end (parser
build, codegen build, fixed-point bootstrap, full test + benchmark
suites). MSVC isn't supported because the runtime relies on POSIX
assumptions (`<ucontext.h>` for `Fiber`, `<sys/mman.h>` for the
regexp engine's executable buffers, GCC's `__attribute__((cleanup))`
for the GC root stack); a port would either replace those or guard
them behind compile-time switches.

## Limitations

- **No eval**: `eval`, `instance_eval`, `class_eval`
- **No dynamic metaprogramming**: `send`, `method_missing`, dynamic
  `define_method`
- **No threads**: `Thread`, `Mutex` (Fiber is supported)
- **No encoding**: assumes UTF-8/ASCII
- **No general lambda calculus**: deeply nested `-> x { }` with `[]` calls

A few cases deliberately diverge from CRuby because the CRuby behavior needs a
feature Spinel does not implement (e.g. `Integer#**` with a negative exponent
raises instead of returning a `Rational`). These are listed in
[docs/INCOMPATIBILITIES.md](docs/INCOMPATIBILITIES.md).

## Dependencies

- **Build time**: [libprism](https://github.com/ruby/prism) (C library),
  CRuby (bootstrap only)
- **Run time**: None. Generated binaries need only libc + libm.
- **Regexp**: Built-in engine, no external library needed.
- **Bigint**: Built-in (from mruby-bigint), linked only when used.

## Contributing

Contributions are welcome. The issue tracker doubles as the roadmap —
anything open is fair game; the most useful entry points are
reproducer-shaped bug reports (a 5-line Ruby that fails in Spinel but
passes in CRuby) and codegen fixes that close one such report.

Workflow:

- Open a focused PR. Small and contained merges faster than sweeping
  refactors.
- Make `make` close (`gen2.c == gen3.c`) and `make test` / `make bench`
  pass before pushing.
- Add a regression test under `test/` for any fix or new feature; the
  harness compares Spinel's output against CRuby on the same source,
  so the test usually doesn't need to assert anything beyond `puts`.
- Reference issues with `Closes #N` / `Fixes #N` / `Refs #N` trailers
  in the commit message.
- If the work was assisted by an AI, add a `Co-Authored-By:` trailer
  for the assistant alongside any human co-authors. The maintainer's
  own AI-assisted commits use `Co-Authored-By: Claude Opus 4.8`.

Adjacent ecosystem (community-built, not part of this repo):

- [rubocop_spinel](https://github.com/gurgeous/rubocop_spinel) —
  a RuboCop custom cop that flags Ruby code Spinel doesn't yet
  support.
- [spinel-dev](https://github.com/OriPekelman/spinel-dev): developer
  tooling for debugging Spinel builds (a `spinel doctor` health check, a
  CRuby-vs-Spinel value bisector for silent miscompiles, a minimal-repro
  reducer, a ruby-lsp type addon, and perf/flamegraph analysis).
- [spinelgems](https://github.com/OriPekelman/spinelgems): a survey of
  which RubyGems compile and run under Spinel, plus bundler-spinel, a
  Bundler plugin that vendors and compatibility-gates `Gemfile`
  dependencies.

## History

Spinel was originally implemented in C (branch `c-version`), then
rewritten in Ruby (branch `ruby-v1`), then rewritten again in a
self-hosting Ruby subset (preserved on the `self-host` branch). The
current `master` is a fresh C implementation of the analyze and codegen
stages, which builds far faster than the self-hosted backend while
producing equivalent output; the self-hosting Ruby version remains
in-tree as a regression oracle (see [Self-Hosting](#self-hosting-legacy-oracle)).

## License

MIT License. See [LICENSE](LICENSE).
