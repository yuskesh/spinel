# spinelgems — requirements (draft for discussion)

Status: requirements draft, not yet implemented. Companion to
[require.md](require.md) (the resolution mechanism packages plug into) and the
"Planned" section there, which this document supersedes and expands.

Naming (decided): the ecosystem is **spinelgems**; one unit is a **gem** (a
"spinelgem" where CRuby gems need distinguishing). One manifest filename,
`spinelgem.toml`, serves both a gem and an application (cargo-style: the
application is simply the gem at the compile root). CLI surface is
`spinel gem <cmd>`. The community compatibility catalog of the same name is
the intended seed for the Phase-2 index (see R5/R8) — to be coordinated with
its author rather than renamed around. The rest of this document says
"package" generically; read it as "gem".

## 1. Problem

External libraries currently live as `.rb` files under `lib/`, side by side
with the C runtime (`sp_*.c/h`) and the bundled stdlib (`set.rb`, `erb.rb`,
`optparse.rb`, `forwardable.rb`). Consequences:

- **No boundary.** A user cannot tell what is runtime (must never be touched),
  what is stdlib (versioned with the compiler), and what is a library (could be
  replaced, upgraded, or written by them).
- **No identity.** A library has no name, version, or dependency declaration;
  "installing" one means copying files into the compiler tree.
- **No sharing.** There is no way to publish a library or consume someone
  else's except by hand, and nothing records what an application depends on.

## 2. Reference points

**RubyGems/Bundler** gives the *consumer model* worth copying: a per-project
manifest declaring dependencies, a lockfile making builds reproducible, a flat
namespace of package names, and `require "name"` as the only thing library
consumers write. Its *mechanism* (runtime `require` onto a `$LOAD_PATH`,
dynamic .so extensions, install-time code execution) does not fit an AOT
whole-program compiler.

**mrbgems** gives the *producer model* worth copying: a gem is a source tree
(`mrblib/*.rb` + `src/*.c` + `include/`) compiled *into* the final binary, with
a small spec declaring sources, dependencies, and compile/link flags. Its
consumer model (gems selected in the interpreter's `build_config.rb`, baked
into the VM for every program) does not fit either: Spinel dependencies are
per-application, not per-toolchain.

Spinel therefore wants: **RubyGems' per-app consumer surface on mrbgems'
compile-into-the-binary producer model.**

## 3. Constraints peculiar to Spinel

These shape everything below.

- **C1 — whole-program AOT.** All sources are known at compile time; `require`
  is textual splicing. Packages are *source* inputs to the one compile: the
  type inference specializes a package's code per application (the same
  library compiles to different C in different apps). There is no binary
  package artifact and no ABI.
- **C2 — subset language.** Not every gem compiles under Spinel. Compatibility
  is a first-class, testable property (the ~189k-gem probe corpus), not a
  footnote.
- **C3 — C is reachable two ways.** FFI (`ffi_lib`/`ffi_func`, linking an
  external library) and, planned, in-TU C carried by the package itself. The
  runtime headers stay additive-only; package C must not mutate runtime
  internals.
- **C4 — the require-gate is the resolution point.** `require "name"` already
  resolves bundled stdlib, native features, and `-I` roots, and gates typed
  surfaces on it. Packages are one more provider behind the same gate — not a
  second mechanism.
- **C5 — types cross package boundaries.** Inference runs over the spliced
  whole program; a package's poly-dispatch arms must be instantiation-gated by
  what the *application* actually constructs (the package-identity work), and
  a package may pin its public surface with `.rbs` to keep inference stable
  across callers.

## 4. Requirements

### R1 — separation of layers

Three layers, physically separated and named:

| layer | contents | ships with | user-visible? |
|---|---|---|---|
| runtime | `sp_*.c/h`, archives | the compiler | never edited, not require-able |
| stdlib | require-gated features (`set`, `erb`, `json`, …) | the compiler, versioned with it | via `require`, no manifest entry |
| packages | everything else | fetched/vendored per project | via `require` + manifest entry |

`lib/` reorganizes to make the boundary visible (e.g. `lib/runtime/` vs
`lib/stdlib/`); the bundled pure-Ruby stdlib becomes ordinary packages
pre-installed with the compiler, proving the package format on day one
(dogfooding requirement).

### R2 — package format

A package is a directory (typically a git repo):

```
mypkg/
  spinelgem.toml      # manifest (name, version, deps, provides)
  lib/                # Ruby sources; lib/<feature>.rb per provided feature
  src/                # optional C sources (in-TU or objects; see R6)
  sig/                # optional .rbs pinning the public surface
  test/               # runnable under `spinel gem test`
  LICENSE / README.md
```

Manifest fields (minimum): `name`, `version` (semver), `provides` (feature
names its `lib/` satisfies; defaults to `name`), `dependencies` (name +
version constraint), `spinel` (compiler version constraint), plus `license`,
`source`. C-carrying packages add a `[native]` table (sources, cflags, libs —
the FFI DSL remains usable *inside* the Ruby sources for external libraries).

Nothing in the manifest is executable. **Fetching or vendoring a package runs
no package code** (contrast: gem native extensions). Compilation of package C
happens only as part of building an application that depends on it.

### R3 — consumer surface

- A project manifest at the application root (`spinelgem.toml`, `[gem]` +
  `[dependencies]` tables) declaring name-and-constraint pairs; sources:
  index name, git URL + ref, or local path.
- A lockfile (`spinelgem.lock`) with exact versions + content hashes;
  committed; builds use only the lockfile.
- In code, consumers write `require "name"` — nothing else. Resolution order:
  runtime-native feature → stdlib → project packages (lockfile) → `-I` roots.
  A `require` satisfied by no layer stays the existing compile-time LoadError.
- Zero-manifest escape hatch stays: `-I` roots keep working for scripts and
  experiments; a manifest is only needed for versioned/fetched dependencies.

### R4 — resolution & versioning semantics

- Flat package namespace; one version of a package per application (no
  side-by-side duplicate versions — whole-program compilation makes two
  copies of the same classes a conflict, not an isolation feature).
- Semver constraints (`~>` pessimistic operator included); resolution failures
  are compile-stopping errors listing the conflicting constraint chain.
- Dependency graph must be acyclic; `require` between packages goes through
  the same gate (a package's `require "foo"` resolves against *its* declared
  dependencies + stdlib, so undeclared cross-package reach is an error —
  mrbgems does not enforce this and suffers for it).

### R5 — distribution (phased)

- **Phase 1: no registry.** Local paths + git URLs pinned by the lockfile
  hash. `spinel gem vendor` copies the resolved tree into `vendor/` for
  offline/hermetic builds; vendored trees are read-only inputs (build
  artifacts never land next to package sources).
- **Phase 2: an index**, not a server: a git-hosted name→(repo, versions)
  index (cargo-style) so `[dependencies] foo = "~> 1.2"` works without URLs.
- **Name policy** for gems that also exist on rubygems.org: same name means
  "the same library, possibly a subset-compatible port"; forks/ports that
  diverge must rename (`foo-spinel`). The probe corpus can seed the initial
  index with gems that already compile unmodified.

### R6 — C in packages

- Package C compiles into the application's build via a build cache keyed by
  (package, version, toolchain, flags) — never into the package tree.
- Two supported shapes: **FFI** to an external installed library (existing
  DSL, package declares `libs` so the link line is derivable), and **carried
  C** (`src/*.c` + headers) compiled as separate TUs and linked in. In-TU
  splicing (single-TU inlining for cross-TU optimization) is an optimization
  behind the same declaration, not a third user-facing shape.
- Carried C sees a *stable, documented* runtime surface (a `spinel/runtime.h`
  umbrella with the public sp_ API); packages including internal headers get
  no compatibility promise. Runtime stays additive-only from a package's
  perspective.

### R7 — type inference across packages

- Package sources are spliced and inferred with the application (C1); no
  pre-compiled types.
- A package may ship `sig/*.rbs` to pin its public methods, giving stable
  diagnostics at the package boundary and protecting the package's inferred
  interior from caller-driven widening (the existing `--rbs` pin machinery).
- Poly-dispatch arms contributed by package classes are instantiation-gated:
  an application that never constructs a package class pays nothing for it
  (dead classes prune like dead user code today).
- The compile reports which package a diagnostic originates from
  (`pkg-name-1.2.0/lib/foo.rb:12:` prefixes, same #line machinery).

### R8 — compatibility as metadata

- `spinel gem test` runs a package's `test/` under the current compiler; the
  manifest records the last-passing compiler version.
- The index (Phase 2) records probe results per compiler release, so `spinel
  gem add` can warn "compiles at 0.9, fails at 0.10" before fetching. This is
  the productized form of the existing corpus reprobes.

### R9 — tooling

`spinel gem <cmd>`: `init` (write a project manifest), `add`/`remove` (edit +
re-resolve + update lockfile), `vendor`, `test`, `new` (scaffold a package).
All offline-capable given a vendor tree; no daemon, no per-machine state
outside an XDG cache directory.

## 5. Non-goals

- **Runtime loading** of any kind (no dlopen, no late require).
- **Binary package distribution** (no ABI exists; C1).
- **Side-by-side versions** of one package in one application (R4).
- **Install-time code execution** (R2); packages compute nothing at fetch.
- **A hosted registry service** in early phases (R5's git index instead).
- Replacing FFI: it remains the boundary to *external* native libraries.

## 6. Open questions

1. **Stdlib carve-out granularity.** Which of the require-gated features move
   from compiler tree to pre-installed gems first (`erb`/`optparse`/`set`
   are the easy three; `json`/`stringio` are C-backed and test R6).
2. **Version skew between compiler and gems.** Is the compiler version
   constraint (`spinel = "~> 0.9"`) hard (refuse) or advisory (warn)?
3. **Index coordination.** Concrete shape of seeding the Phase-2 index from
   the community spinelgems catalog (and who owns the name registry).

## 7. Decided

- Manifest is **TOML**, never executable (R2's no-code-at-fetch guarantee).
- Ecosystem name **spinelgems**; unit "gem"; one `spinelgem.toml` for gems
  and applications alike; CLI `spinel gem <cmd>`.
- rubygems.org interop is **name policy only** (R5): same name means the same
  library (possibly a subset-compatible port); divergent forks rename.
  Publishing spinel gems on rubygems.org itself is out of scope.
