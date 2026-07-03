# spinelgems & spin — design

The design record for Spinel's package system (**spinelgems**) and its
project tool (**`spin`**). The user-facing guide is [../spin.md](../spin.md);
this document is the *why* and the *contract*: the constraints, the
requirements (R1–R9), the resolution semantics, and what remains open.

Status: implemented through M3 — scaffold, path/git/index dependencies with
MVS selection, `gem.lock`, vendor + offline, snapshot tests, carried native
C, `list`/`tree`/`search`. The hermetic end-to-end check
(`tools/spin_e2e.sh`) runs inside `make check`. Sections below note the
pieces that are still specification.

## 1. Problem

External libraries used to live as `.rb` files under `lib/`, side by side
with the C runtime (`sp_*.c/h`) and the bundled stdlib. Consequences: no
boundary (what is runtime, stdlib, or replaceable library?), no identity
(no name, version, or dependency declaration), no sharing (installing a
library meant copying files into the compiler tree).

## 2. Reference points

**RubyGems/Bundler** gives the *consumer model* worth copying: a per-project
manifest, a lockfile, a flat namespace, and `require "name"` as the only
thing consumers write. Its *mechanism* (runtime `require` onto a load path,
dynamic .so extensions, install-time code execution) does not fit AOT.

**mrbgems** gives the *producer model* worth copying: a gem is a source tree
compiled *into* the final binary, with a small spec. Its consumer model
(gems baked into the VM per toolchain) does not fit either: Spinel
dependencies are per-application.

Spinel wants **RubyGems' per-app consumer surface on mrbgems'
compile-into-the-binary producer model.**

## 3. Constraints peculiar to Spinel

- **C1 — whole-program AOT.** All sources are known at compile time;
  `require` is textual splicing. Packages are *source* inputs to the one
  compile: inference specializes a package's code per application. There is
  no binary package artifact and no ABI.
- **C2 — subset language.** Not every gem compiles under Spinel.
  Compatibility is a first-class, testable property (the ~189k-gem probe
  corpus), not a footnote.
- **C3 — C is reachable two ways.** FFI (`ffi_lib`/`ffi_func`) to external
  libraries, and carried C inside the gem tree. The runtime headers stay
  additive-only; package C must not mutate runtime internals.
- **C4 — the require-gate is the resolution point.** `require "name"`
  already resolves bundled stdlib, native features, and `-I` roots, and
  gates typed surfaces on it. Packages are one more provider behind the
  same gate — not a second mechanism. Inside a `spin` project the gate is
  strict (`SPINEL_REQUIRE_GATE=1`): the dependency universe is fully known,
  so an unresolvable `require` is a hard error, and stdlib features need
  their `require` just as in CRuby.
- **C5 — types cross package boundaries.** Inference runs over the spliced
  whole program; a package's poly-dispatch arms must be instantiation-gated
  by what the *application* constructs (open work), and a package may pin
  its public surface with `.rbs` sidecars.

## 4. Requirements

### R1 — separation of layers

| layer | contents | ships with | user-visible? |
|---|---|---|---|
| runtime | `sp_*.c/h`, archives | the compiler | never edited, not require-able |
| stdlib | require-gated features (`set`, `erb`, `json`, …) | the compiler | via `require`, no manifest entry |
| gems | everything else | fetched/vendored per project | via `require` + manifest entry |

The boundary is directory-level. *Still specification:* the carve-out that
reduces `lib/` to runtime C only and moves the bundled pure-Ruby stdlib
into a gems-only `gems/<name>/` directory (proving the gem format on the
compiler's own stdlib). Today the stdlib still lives in `lib/`. Gems-only
directories that do exist: `vendor/gems/<name>-<version>/` in a project,
`$XDG_CACHE_HOME/spinel/gems/<name>-<version>/` for fetches.

### R2 — package format (implemented)

A gem is a directory, typically a git repo named `spinel-<name>` by
publishing convention; the gem *name* carries no prefix because the name is
the `require` string. There are no per-language role directories: **role is
carried by extension and the gem root is the require root**. `.rb` compiles
and defines the require namespace (`mypkg.rb`, subfeatures under `mypkg/`),
`.rbs` is an optional sidecar, `.c`/`.h` are carried native sources (R6).
Reserved directory names, excluded from the require namespace: `test/`,
`bin/`, `build/`, `vendor/`.

**Executables.** Each `bin/<name>.rb` is an executable and its own
whole-program compile root, spliced with the gem's library sources and
resolved dependencies (cargo's `src/bin/*.rs` shape). **An application is a
gem with executables** — same manifest, no separate project kind, and the
`[gem]` identity table is optional for applications (name defaults to the
directory basename; publishing is what makes identity mandatory).
Dependents of a gem never compile its `bin/`.

**Tests.** Each top-level `test/*.rb` is one test program through exactly
the `bin/` mechanics. Pass/fail is the compiler-repo oracle convention: a
committed `.expected` snapshot diffs against stdout; with no snapshot the
same file runs under `ruby` and diffs directly — the subset-parity check.
`spin test --regen` refreshes snapshots. Subdirectories of `test/` hold
`require_relative` helpers, not entries.

**Manifest** (`gem.toml`) is TOML and never executable: fetching or
vendoring a gem runs no gem code, and compilation of gem C happens only
while building a dependent application. Implemented fields: `[gem]
name`/`version`, `[dependencies]`. *Still specification:*
`[dev-dependencies]`, `provides` (feature names beyond the gem's own),
`spinel` (compiler version constraint — reserved until the toolchain is
versioned), `[native]` cflags/libs (carried C currently needs no manifest
entry; external libraries use the `ffi_lib` DSL in the Ruby source), and
`[build] spinel-flags`.

### R3 — consumer surface (implemented)

- `gem.toml` declares dependencies; sources are an index constraint string,
  `{ git = URL[, ref = R] }`, or `{ path = DIR }`.
- `gem.lock` records the machine-resolved result: exact versions including
  transitive dependencies, full commit SHAs for git/index sources.
  Reproducibility matters more than in CRuby: under whole-program inference
  a dependency bump can change whether the application *compiles*.
  Applications commit it; libraries do not. Absent a lock, builds resolve
  from the manifest (deterministic — see R5/MVS).
- In code, consumers write `require "name"` — nothing else. Resolution
  order: runtime-native feature → stdlib → project gems → `-I` roots.
- The zero-manifest escape hatch stays: hand-written `spinel -I` invocations
  have identical semantics; a manifest is only needed for
  versioned/fetched dependencies.

### R4 — resolution & versioning semantics

- Flat namespace; **one version of a gem per application** (two copies of
  the same classes are a whole-program conflict, not an isolation feature).
- The dependency graph is walked transitively (a fetched gem's own
  `[dependencies]` resolves too) and must be acyclic.
- *Still specification:* feature-namespace ownership enforcement (two gems
  providing overlapping features as a named resolution error), and
  undeclared-cross-gem-require checking below resolution granularity — the
  latter needs per-root provenance that plain `-I` does not carry, layered
  on top of `-I` when it moves into the compiler.

### R5 — distribution (implemented through the index)

- **Phase 1: no registry.** Local paths + git URLs pinned by the lockfile
  SHA. `spin vendor` copies the resolved tree into `vendor/gems/`;
  `SPIN_OFFLINE=1` builds from cache/vendor only. Vendored trees are
  read-only inputs.
- **Phase 2: an index, not a server** (implemented): a git repository —
  <https://github.com/matz/spinel-index> by default, `SPIN_INDEX`
  overrides, `file://` works — mapping names to repos and releases. One
  TOML file per gem, read by the same reader as `gem.toml`:

  ```toml
  # gems/<name>.toml
  name = "ansi"
  repo = "https://github.com/x/spinel-ansi"

  [[release]]
  version = "1.2.0"
  ref = "a94a8fe5cc..."   # full commit SHA in `repo`
  ```

- **Name policy**: same name as a rubygems.org gem means "the same library,
  possibly a subset-compatible port"; divergent forks rename
  (`foo-spinel`). The probe corpus is the intended seed for the index
  (coordination still open).
- *Still specification:* `spin publish` (index PR automation).

**Selection is MVS** (decided, implemented): among releases admitted by the
constraint (`~>` pessimistic, `>=`, exact, `*`), every gem resolves to the
*lowest* admissible version.

- Deterministic without a lockfile — which is what makes `gem.lock`
  droppable for libraries instead of load-bearing.
- No SAT solving. Constraint gathering is first-encounter: a later
  conflicting constraint on an already-resolved gem is an error, not a
  re-solve.
- Not auto-riding the newest release is intentional under whole-program
  inference; upgrades are opt-in and diffable.

Order of authority: `gem.lock` when it satisfies the manifest (a pinned
version that no longer satisfies a changed constraint is reselected with a
warning, and the next `spin lock` rewrites the pin); `vendor/gems/` then the
cache for sources; path deps always read live and are recorded unpinned.
Fetch materializes the exact release SHA — direct SHA fetch with a
full-clone + checkout fallback — and dies on mismatch. *Still
specification:* `spin lock --update` and `--frozen` (CI mode).

### R6 — C in packages (implemented)

- Carried C is discovered by extension: every `.c` in the gem tree (outside
  `build/`, `vendor/`, `test/`) compiles into the shared cache
  `$XDG_CACHE_HOME/spinel/native/<gem>-<version>-<cc>/` — never into the
  package tree — and the objects reach the compiler via its repeatable
  `--link` flag; `spinel` itself never compiles gem C. Objects rebuild when
  any of the gem's `.c`/`.h` is newer; the gem tree and the compiler's
  runtime headers are on the include path; `CC` selects the toolchain.
- The second shape, FFI to an external installed library, stays on the
  Ruby-side `ffi_lib`/`ffi_func` DSL (its SPINEL_LINK/SPINEL_CFLAGS markers
  already reach the link line). In-TU splicing of carried C is a possible
  future optimization behind the same declaration, not a third shape.
- *Still specification:* the `spinel/runtime.h` umbrella defining the
  stable public sp_ surface for carried C; today gem C sees the same
  headers as the generated TU, with no compatibility promise on internals.

### R7 — type inference across packages

- Package sources are spliced and inferred with the application (C1); no
  pre-compiled types. `.rbs` sidecars pin public surfaces via the existing
  `--rbs` machinery. Diagnostics carry file:line positions through the
  `#line` machinery.
- *Open:* instantiation-gating of package-contributed poly-dispatch arms
  (an application that never constructs a package class should not pay for
  it — also the recovery path for the gate-flip's optcarrot cost).

### R8 — compatibility as metadata (specification)

`spin test` already runs a gem's tests under the current compiler; the
recorded last-passing compiler version, per-release probe results in the
index, and "compiles at 0.9, fails at 0.10" warnings in `spin add`/`build`
await toolchain versioning.

### R9 — tooling: `spin`, a separate project tool (implemented)

The compiler CLI stays gcc-like: sources + `-I` roots in, binary out, no
network, no manifest knowledge, no state. `spin` owns everything stateful —
manifest editing, resolution, fetching, vendoring, invoking the compiler —
and every command works offline given a populated cache or vendor tree. No
daemon; no per-machine state outside `$XDG_CACHE_HOME/spinel/`.

- `spin` is written in Spinel and ships beside the compiler (`make
  bin/spin`), dogfooding the language and the gem format. Its dependencies
  are stdlib-only; its TOML reader (`tools/spin/toml.rb`, string-only
  storage, `[table]`/`[[table]]`/inline tables) is the intended seed of a
  future `toml` stdlib feature.
- Subcommands on the compiler CLI were considered and rejected: `spinel
  build` is ambiguous against `spinel build.rb`, and the compiler binary
  should not carry TOML/git/network code.
- Compiler interface: one `-I` per resolved gem in resolution order, plus
  `-I <root>`, plus `--link <obj>` per cached native object, plus
  `SPINEL_REQUIRE_GATE=1`. An unresolved-`require` compile error is wrapped
  with the `spin add` hint that would fix it.
- Known name collisions, accepted: Fermyon's wasm tool and the SPIN model
  checker also install `spin`; distro packages may need a `spinel-spin`
  package name even though the binary stays `spin`.

## 5. Project model

- **Project root** = the nearest ancestor directory containing `gem.toml`
  (upward walk; commands run from any subdirectory).
- Build artifacts are per-project and disposable: `build/bin/<target>`,
  `build/test/<name>` (`spin clean` removes `build/`). `spin` writes
  nowhere else in the project except `gem.toml` (`add`/`remove`),
  `gem.lock`, and `vendor/gems/`.
- Rebuild tracking is the newest-input-mtime of the project and its
  dependency trees (`.rb`/`.rbs`/`.c`/`.h`/`gem.toml` plus the compiler
  binary) against each output — a flat input set; there is deliberately no
  per-file dependency graph, because type specialization spans every
  source. *Aspiration:* content-hash stamps (the compiler-repo scheme)
  instead of mtimes. Gem C is cached across projects (R6); `spin` runs no
  arbitrary build tasks (no rake surface) — projects with bespoke steps
  call `spin` from their own build system.
- Output: one line per phase; `spin list --json` / `spin tree --json` are
  the machine surface. *Still specification:* `-q`/`-v`, distinct exit
  codes (currently 0/1), test parallelism (`-j`), `spin install`,
  `spin clean --cache`, and `spin remove` refusing while another dependency
  still requires the gem.

## 6. gem.lock

TOML, one table per resolved gem, written by `spin add`/`remove`/`lock`,
consumed by every build; something you *diff*, never edit:

```toml
[lock.ansi]
version = "0.1.0"        # from the gem's own gem.toml at the pinned ref
git = "https://…"        # git and index sources: repo URL
ref = "<commit SHA>"     # always a full SHA, never a branch name

[lock.local]
version = "0.0.0"
path = "../spinel-local" # path deps are recorded but never pinned
```

Resolution against a lock is **verification, not selection**: the lock pins
git/index sources to their SHAs; constraints are re-checked against it, and
only a constraint the pin no longer satisfies triggers reselection (with a
warning). Content integrity rides on the git SHA; tree hashing for
non-git transports would arrive with any such transport.

## 7. Non-goals

- **Runtime loading** of any kind (no dlopen, no late require).
- **Binary package distribution** (no ABI exists; C1).
- **Side-by-side versions** of one gem in one application (R4).
- **Install-time code execution** (R2); gems compute nothing at fetch.
- **A hosted registry service** (the git index instead).
- Replacing FFI: it remains the boundary to *external* native libraries.

## 8. Implementation notes

`tools/spin.rb` (+ `tools/spin/toml.rb`), compiled to `bin/spin`;
`tools/spin_e2e.sh` is the hermetic end-to-end check (`make spin-check`,
part of `make check`): scaffold, path/git/index deps, MVS, lock schema,
add/remove/search, tests in both oracle modes, carried C with cache reuse
and staleness, the require gate, vendor and offline with and without a
lock.

Being written in the subset, spin also serves as a trap log; the ones its
code works around deliberately (see the comments in place): nil-carrying
`String` returns flow through `""` instead (no first-class `string?`),
arrays-of-tuples and `[]` accumulator arguments go poly-array and poison
string parameters downstream (tab/newline-packed record strings instead),
`\0` cannot separate C-string keys (`\x01` instead), and a method named
`json_str` collides with the runtime's `sp_json_str` symbol.

## 9. Open points

1. **Stdlib carve-out** (R1): which require-gated features move from the
   compiler tree into pre-installed gems first (`erb`/`optparse`/`set` are
   the easy three; `json`/`stringio` are C-backed and exercise R6), and the
   `gems/` directory itself.
2. **Index seeding** from the probe corpus, and coordination with the
   community spinelgems compatibility catalog (who owns the name registry).
3. **Toolchain versioning**: the `spinel` manifest constraint, R8 probe
   warnings, and spin↔spinel skew handling all wait on the compiler
   carrying a version.
4. **`spin publish`** (index PR automation).
5. R4 enforcement below resolution granularity (per-root provenance on top
   of `-I`).
6. Native-cache eviction (currently: never).
7. `spin lock --update`, `--frozen`, `--dev`/`[dev-dependencies]`,
   `spin install`, `-q`/`-v`/`-j`, `spin clean --cache`.
