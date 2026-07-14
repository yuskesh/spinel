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
serialization step in between. See [docs/internals/AST.md](docs/internals/AST.md)
for the text AST format the parser emits and the analyzer consumes.

## Quick Start

```bash
# Fetch libprism sources (from the prism gem on rubygems.org):
make deps

# Build everything (the compiler `spinel` and the project tool `spin`):
make
sudo make install     # optional: puts spinel and spin on PATH

# Start a project:
spin new myapp && cd myapp
spin run              # compile bin/myapp.rb and run it
```

`spin` is the day-to-day interface — cargo/mix style. It scaffolds
projects, resolves dependencies, drives the compiler, and runs tests; no
Makefile, no hand-written `-I` flags:

```bash
spin add ansi --version "~> 1.0"   # from the spin package index
spin add mylib --path ../mylib     # or a local checkout / --git URL
spin test                          # snapshot tests (CRuby is the oracle)
spin build && ./build/bin/myapp
```

Dependencies (packages) are source trees compiled into your binary — no
runtime loading, no extension builds; a package can even carry `.c` files.
See **[docs/spin.md](docs/spin.md)** for the full guide, including how to
write and publish a library.

### Single files: the compiler directly

`spinel` is the underlying compiler — gcc-like, one job — and stays the
right tool for single-file scripts and experiments:

```bash
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

./spinel hello.rb
./hello               # prints 5702887 (instantly)
```

```bash
./spinel app.rb              # compiles to ./app
./spinel app.rb -o myapp     # compiles to ./myapp
./spinel app.rb -c           # generates app.c only
./spinel app.rb -S           # prints C to stdout
./spinel -E app.rb a b c     # compile, run with ARGV=[a, b, c], discard binary
./spinel -e 'puts 42'        # compile inline source
./spinel app.rb --int-overflow=wrap   # +/-/* wrap silently instead of raising
```

`./spinel` is a single native binary (`build/spinel`; the repo-root
`spinel` is a convenience symlink `make` creates) that parses, infers
types, emits C, invokes `cc` to link it, and can run the result — no shell
wrapper or chained helper binaries, no network, no manifest knowledge
(that separation is what keeps builds hermetic; `spin` owns the stateful
side). It supports the full option set, including `--rbs DIR` (RBS-seeded
inference) and the `--emit-rbs` / `--emit-types` / `--emit-symbol-map`
analysis modes.

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
is at worst a no-op. See [docs/rbs-extract.md](docs/rbs-extract.md)
for the supported subset.

## Self-Hosting (history)

Spinel began as a compiler written in a Ruby subset that compiled
itself. The C compiler has since fully replaced that backend, and the C
build is what `master` ships and what the gate (`make test` / `make
bench` / `make optcarrot`) checks. The original self-hosting Ruby
implementation is preserved on the [`self-host`](../../tree/self-host)
branch for historical reference; it is no longer carried in `master`.

## Benchmarks

1,744 tests pass. 58 benchmarks pass.
Geometric mean: **~5.8x faster** than Ruby 4.0.4 with `--yjit` across
the 28 benchmarks below. Baseline is CRuby 4.0.4 (stable), run with
`--disable-gems` and with `--yjit` for the JIT column. Each timing is
the best of three wall-clock runs; sub-10 ms cells are dominated by
interpreter / runtime startup and should be read as "noise floor."

### Computation

| Benchmark | Spinel | Ruby 4.0.4 | + YJIT | Speedup vs YJIT |
|---|---|---|---|---|
| mandelbrot | 16 ms | 897 ms | 900 ms | 56.2x |
| matmul | 6 ms | 192 ms | 197 ms | 32.8x |
| nqueens | 6 ms | 143 ms | 137 ms | 22.8x |
| partial_sums | 54 ms | 757 ms | 741 ms | 13.7x |
| sieve | 19 ms | 257 ms | 253 ms | 13.3x |
| life (Conway's GoL) | 17 ms | 487 ms | 222 ms | 13.1x |
| sudoku | 3 ms | 63 ms | 32 ms | 10.7x |
| fannkuch | 1 ms | 10 ms | 10 ms | 10.0x |
| fasta (DNA seq gen) | 2 ms | 9 ms | 9 ms | 4.5x |
| fib (recursive) | 19 ms | 377 ms | 47 ms | 2.5x |
| tak | 26 ms | 310 ms | 45 ms | 1.7x |
| tarai | 21 ms | 249 ms | 36 ms | 1.7x |
| ackermann | 26 ms | 272 ms | 36 ms | 1.4x |

### Data Structures & GC

| Benchmark | Spinel | Ruby 4.0.4 | + YJIT | Speedup vs YJIT |
|---|---|---|---|---|
| so_lists | 19 ms | 248 ms | 153 ms | 8.1x |
| huffman (encoding) | 6 ms | 37 ms | 38 ms | 6.3x |
| splay tree | 9 ms | 107 ms | 37 ms | 4.1x |
| rbtree (red-black tree) | 17 ms | 330 ms | 63 ms | 3.7x |
| linked_list | 40 ms | 182 ms | 138 ms | 3.5x |
| binary_trees | 4 ms | 24 ms | 12 ms | 3.0x |
| gcbench | 362 ms | 2_180 ms | 937 ms | 2.6x |

### Real-World Programs

| Benchmark | Spinel | Ruby 4.0.4 | + YJIT | Speedup vs YJIT |
|---|---|---|---|---|
| ao_render (ray tracer) | 82 ms | 1_665 ms | 591 ms | 7.2x |
| str_concat | 1 ms | 6 ms | 6 ms | 6.0x |
| pidigits (bigint) | 1 ms | 6 ms | 6 ms | 6.0x |
| bigint_fib (1000 digits) | 1 ms | 5 ms | 6 ms | 6.0x |
| template engine | 71 ms | 446 ms | 317 ms | 4.5x |
| json_parse | 29 ms | 189 ms | 128 ms | 4.4x |
| io_wordcount | 16 ms | 47 ms | 42 ms | 2.6x |
| csv_process | 144 ms | 430 ms | 378 ms | 2.6x |

A few notes on what YJIT does and doesn't change. On some integer-loop
workloads (mandelbrot, nqueens, matmul, partial_sums, sieve) YJIT's
numbers are essentially identical to interpreted Ruby; the benchmark
is bound by integer / float operations that the interpreter already
runs at native speed. On call-heavy code (ackermann, fib, tarai, tak,
rbtree) YJIT gives a real 5-8x lift, but Spinel still wins by ahead-
of-time specialization. The narrowest margins are the tiny recursive
kernels (`ackermann`, `tarai`, `tak`, `fib`), where YJIT inlines the
recursive call site so well that Spinel's compiled C wins by only
1.4-2.5x.

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

**Types**: Integer, Float, String (immutable + mutable), Symbol, Array,
Hash, Range, Time, StringIO, File, Regexp, MatchData, Complex, Rational,
Bigint (auto-promoted), Enumerator, Set, Fiber, Thread, Mutex, Queue,
SizedQueue, ConditionVariable, Marshal (dump/load). Polymorphic values
via tagged unions. Nullable object types (`T?`) for self-referential
data structures (linked lists, trees).

**Inspect / `p`**: `Object#inspect` produces CRuby-byte-identical
output across the whole type surface -- primitives, typed and
heterogeneous arrays, every Hash variant, Range, Struct, and
user-class instances (`#<Name:0x... @ivar=...>`, or a user-defined
`inspect` if present), including values reached through a polymorphic
(tagged-union) binding. `p obj`, `obj.inspect`, `obj.to_s`, and
`"#{obj}"` interpolation all agree.

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

**Fiber**: Cooperative concurrency via a small portable assembly
context switch (x86-64 / arm64; no `ucontext` dependency). `Fiber.new`,
`Fiber#resume`, `Fiber.yield` with value passing, `Fiber#raise`/`#kill`,
external `Enumerator`s and `Enumerator::Lazy` ride the same machinery.
Captures free variables via heap-promoted cells. Per-fiber storage via
`Fiber[:k]` / `Fiber[:k] = v` (and the `Fiber.current[:k]` aliases) —
symbol-keyed poly-valued, lazily allocated, shallow-snapshot inherited
from the parent at `Fiber.new` time.

**Threads**: `Thread` runs with **true parallelism and no GVL** — an
M:N scheduler multiplexes green threads onto OS workers (one per core,
cap with `SPINEL_WORKERS`), with work stealing and a ~10 ms preemption
quantum, over a stop-the-world GC. `Thread.new/#join/#value/#alive?`,
`Thread.current/main/list/pass`, thread-locals, and the synchronization
primitives `Mutex` (`#synchronize/#lock/#unlock/#try_lock`), `Queue` /
`SizedQueue` (blocking `#pop`/`#push`), and `ConditionVariable`
(`#wait/#signal/#broadcast`) are supported. Unsynchronized shared
mutation is a data race, as in JRuby/TruffleRuby — see
[docs/thread.md](docs/thread.md) for the model and the full API list.
The threaded runtime is a separate archive linked only when a program
actually uses `Thread`; single-threaded programs keep the byte-identical
fast path.

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
test/                 1,744 feature tests
benchmark/            58 benchmarks
docs/                 User docs (require, FFI, RBS, limitations); internals/ for compiler structure
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

The parser is **src/spinel_parse.c**, which links libprism directly (no
CRuby needed) and emits the text AST the compiler consumes.
`require_relative` is resolved at parse time by inlining the referenced
file.

## Building

```bash
make deps         # fetch libprism into vendor/prism (one-time)
make              # build the compiler (parser + regexp library + spinel) and spin
make test         # run the feature tests (always a fresh run)
make bench        # run benchmarks vs CRuby
make optcarrot    # end-to-end optcarrot integration test
sudo make install # install to /usr/local (spinel and spin in PATH)
make clean        # remove build artifacts
```

Override install prefix: `make install PREFIX=$HOME/.local`

[Prism](https://github.com/ruby/prism) is the Ruby parser used by
`spinel_parse`. `make deps` downloads the prism gem tarball from
rubygems.org and extracts its C sources to `vendor/prism`. If you
already have the prism gem installed, the build auto-detects it; you
can also point at a custom location with `PRISM_DIR=/path/to/prism`.

CRuby is not needed to build or run the C compiler -- only as an
optional parser fallback (the Prism-gem path) and in the test harness,
which compares Spinel's output against CRuby on the same source.

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

The runtime is POSIX-flavoured and targets POSIX platforms:

| Platform | Status | Compiler |
|---|---|---|
| Linux (x86-64, arm64) | Supported | gcc, clang |
| macOS (Intel, Apple Silicon) | Supported | clang |
| *BSD | Expected to work; not in CI | clang |
| Windows | Use [WSL](https://learn.microsoft.com/windows/wsl/) (builds/runs as Linux) | gcc, clang |

Every PR runs `ubuntu-latest / gcc`, `ubuntu-latest / clang`, and
`macos-latest / clang` jobs end-to-end (parser build, codegen build,
full test + benchmark suites). Native Windows (MinGW / MSVC) is not
supported: the runtime relies on POSIX assumptions (`pthread` for the
threaded runtime, `<sys/mman.h>` for the regexp engine's executable
buffers, GCC's `__attribute__((cleanup))` for the GC root stack, and
GCC/Clang inline assembly for the Fiber context switch). Windows users
run Spinel under WSL, where it builds and runs as a native Linux
toolchain.

## Limitations

- **No eval**: `eval`, `instance_eval("str")`, `class_eval("str")`
  (the block forms *are* compiled)
- **No dynamic metaprogramming**: `method_missing`, `define_method` /
  `send` with a runtime-computed name (a literal `send(:name)` and
  literal `define_method` do work)
- **No encoding**: assumes UTF-8/ASCII
- **No general lambda calculus**: deeply nested `-> x { }` with `[]` calls

A few cases deliberately diverge from CRuby because the CRuby behavior needs a
feature Spinel does not implement (e.g. `Integer#**` with a negative exponent
raises instead of returning a `Rational`). These are catalogued under "By
design" in [docs/limitations.md](docs/limitations.md).

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
  tooling for debugging Spinel builds (a CRuby-vs-Spinel value bisector
  for silent miscompiles, a ruby-lsp type addon, and perf/flamegraph
  analysis). The zero-dependency tools — `spinel-doctor` (health check),
  `spinel-reduce` (minimal-repro reducer), and `spinel-flatten` — now
  ship in the box; see [`tools/`](tools/).
- [spin packages](https://github.com/OriPekelman/spin packages): a survey of
  which RubyGems compile and run under Spinel, plus bundler-spinel, a
  Bundler plugin that vendors and compatibility-gates `Gemfile`
  dependencies.

## History

Spinel was originally implemented in C (branch `c-version`), then
rewritten in Ruby (branch `ruby-v1`), then rewritten again in a
self-hosting Ruby subset (preserved on the `self-host` branch). The
current `master` is a fresh C implementation of the analyze and codegen
stages, which builds far faster than the self-hosted backend while
producing equivalent output; the self-hosting Ruby version is preserved
on the `self-host` branch (see [Self-Hosting](#self-hosting-history)).

## License

MIT License. See [LICENSE](LICENSE).
