# spinelgems — requirements (draft for discussion)

Status: requirements draft, not yet implemented. Companion to
[require.md](require.md) (the resolution mechanism packages plug into) and the
"Planned" section there, which this document supersedes and expands.

Naming (decided): the ecosystem is **spinelgems**; one unit is a **gem** (a
"spinelgem" where CRuby gems need distinguishing). One manifest filename,
`gem.toml`, serves both a gem and an application (cargo-style: the
application is simply the gem at the compile root). Project workflows live in a separate tool, `spin` (R9);
the compiler CLI stays compile-only. The community compatibility catalog of the same name is
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

The boundary is directory-level: `lib/` reduces to the runtime C only
(`sp_*.c/h`, archives), and a dedicated top-level `gems/` directory holds the
pre-installed gems — the bundled pure-Ruby stdlib becomes ordinary gems in
there, proving the gem format on day one (dogfooding requirement). Every
directory that holds gems holds *only* gems:

```
<install / compiler repo>
  bin/spinel
  lib/                    # runtime C only
  gems/<name>/            # pre-installed (version = the compiler's)

<project>
  gem.toml (+ gem.lock in applications)
  vendor/gems/<name>-<version>/    # vendored for hermetic builds

~/.cache/spinel/gems/<name>-<version>/   # fetch cache (XDG)
```

### R2 — package format

A package is a directory (typically a git repo). The directory/repository is
named `spinel-<name>` (the mruby-\* convention: discoverable, and visibly a
Spinel port next to a same-named CRuby gem); the gem *name* in the manifest
carries **no prefix**, because the name is the `require` string and
`require "json"` must keep working verbatim (R5's name policy). mrbgems can
conflate the two because mruby has no `require`; Spinel cannot. The prefix is
a scaffolding default (`spin new foo` creates `spinel-foo/`), not a
resolution rule — the index maps names to repos, wherever they live.

There are no per-language role directories (RubyGems' `lib/` is a load-path
unit and mrbgems' `mrblib/`/`src/` split follows its two build pipelines;
Spinel has neither — everything is input to the one compile). **Role is
carried by extension, and the gem root is the require root**, matching the
colocated-directory form in [require.md](require.md): `.rb` compiles and
defines the require namespace, `.rbs` is an optional sidecar next to the file
it pins, `.c`/`.h` are carried native sources (R6). Two reserved
directory names, both excluded from the require namespace: `test/` (runnable
under `spin test`) and `bin/` (executables — see below); manifest, README,
and LICENSE are excluded by extension. A single-file gem is the minimal form:

```
spinel-mypkg/
  gem.toml      # manifest: name = "mypkg" (no prefix), version, deps, provides
  mypkg.rb      # require "mypkg" resolves here
  mypkg.rbs     # optional sidecar pinning the public surface
  mypkg_ext.c   # optional carried C (see R6)
  mypkg/        # subfeatures: require "mypkg/util" -> mypkg/util.rb
  bin/          # executables: each bin/<name>.rb is a compile root
  test/         # runnable under `spin test`
  LICENSE / README.md
```

**Executables.** Gems are not only libraries: RubyGems has `executables`
(rake, rubocop) and mrbgems has `mruby-bin-*`. Here, each `bin/<name>.rb` is
an executable named after the file, and each is its *own whole-program
compile root* — spliced with the gem's library sources and resolved
dependencies, cargo's `src/bin/*.rs` shape. An **application is simply a gem
with executables**; there is no separate project kind and no entry-point
manifest field. `spin build` builds every `bin/` entry, `spin run <name>`
runs one (bare `spin run` when there is exactly one). Because executables
are separate compile roots, a *dependent* of the gem never compiles or pays
for them — the reason mrbgems splits `mruby-bin-*` into separate gems does
not apply, and library + CLI live in one gem.

**Tests.** Each top-level `test/*.rb` is one test program — an independent
compile root through exactly the `bin/` mechanics (gem sources + resolved
dependencies spliced in), no runner framework. Subdirectories of `test/` are
not entries; they hold `require_relative` helpers. Pass/fail is the
compiler's own oracle convention: a committed `test/<name>.rb.expected` is
diffed against the compiled run's stdout (`.rb.err.expected` for stderr);
with no snapshot and a CRuby available, `spin test` runs the same file under
`ruby` and diffs directly — for a port, the test *is* the subset-parity
check. `spin test --regen` refreshes snapshots from CRuby. A non-zero exit
or a diff fails, so assert-and-raise style works unchanged (raise → non-zero
→ fail); test-only helper gems go in `[dev-dependencies]`, resolved for
`spin test` but never for consumers.

Manifest fields (minimum): `name`, `version` (semver), `provides` (feature
names its sources satisfy; defaults to `name`), `dependencies` (name +
version constraint), `dev-dependencies` (test-only, resolved for `spin test`
and never for consumers), `spinel` (compiler version constraint — reserved,
unenforced until the toolchain is versioned), plus `license`,
`source`. C-carrying packages add a `[native]` table (sources, cflags, libs —
the FFI DSL remains usable *inside* the Ruby sources for external libraries).

Nothing in the manifest is executable. **Fetching or vendoring a package runs
no package code** (contrast: gem native extensions). Compilation of package C
happens only as part of building an application that depends on it.

### R3 — consumer surface

- A project manifest at the application root (`gem.toml`, `[gem]` +
  `[dependencies]` tables) declaring name-and-constraint pairs; sources:
  index name, git URL + ref, or local path.
- A lockfile (`gem.lock`) recording the machine-resolved result of the
  manifest's human intent: exact versions *including transitive
  dependencies*, plus content hashes for integrity. Reproducibility matters
  more here than in CRuby — under whole-program inference a dependency's
  minor bump can change whether the application *compiles*, not just how it
  behaves.
- The lockfile is an application artifact, not a requirement: absent a
  `gem.lock`, builds resolve from `gem.toml` (fully deterministic when every
  source is an exact pin); `spin add`/`lock` writes it. Gems that build executables (applications) commit it; pure library gems do
  not (version selection belongs to the consuming
  application — the cargo convention).
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
- **Feature-namespace ownership.** A gem implicitly owns the feature subtree
  under each name it provides: gem `json` owns `json` and `json/*` (the
  natural reading of root-as-require-root — any `json/pure.rb` in its tree is
  a feature). `provides` may declare additional top-level names for the
  CRuby-realistic cases where gem name and require string differ
  (`concurrent-ruby` providing `concurrent`). Two gems in one resolved set
  providing overlapping features is a resolution error, named as such.
  Backend-selection requires from CRuby (`require "json/pure"` forcing the
  pure implementation) are just subfeatures here — and the C-vs-pure
  distinction itself dissolves under AOT: a pure-Ruby implementation compiles
  to native and takes whole-program inference, so `json/pure.rb` may simply
  splice the same implementation while keeping the require string valid.

### R5 — distribution (phased)

- **Phase 1: no registry.** Local paths + git URLs pinned by the lockfile
  hash. `spin vendor` copies the resolved tree into `vendor/` for
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
  C** (`.c`/`.h` sources in the gem tree) compiled as separate TUs and linked in. In-TU
  splicing (single-TU inlining for cross-TU optimization) is an optimization
  behind the same declaration, not a third user-facing shape.
- Carried C sees a *stable, documented* runtime surface (a `spinel/runtime.h`
  umbrella with the public sp_ API); packages including internal headers get
  no compatibility promise. Runtime stays additive-only from a package's
  perspective.

### R7 — type inference across packages

- Package sources are spliced and inferred with the application (C1); no
  pre-compiled types.
- A package may ship `.rbs` sidecars to pin its public methods, giving stable
  diagnostics at the package boundary and protecting the package's inferred
  interior from caller-driven widening (the existing `--rbs` pin machinery).
- Poly-dispatch arms contributed by package classes are instantiation-gated:
  an application that never constructs a package class pays nothing for it
  (dead classes prune like dead user code today).
- The compile reports which package a diagnostic originates from
  (`pkg-name-1.2.0/lib/foo.rb:12:` prefixes, same #line machinery).

### R8 — compatibility as metadata

- `spin test` runs a package's `test/` under the current compiler; the
  manifest records the last-passing compiler version.
- The index (Phase 2) records probe results per compiler release, so `spinel
  gem add` can warn "compiles at 0.9, fails at 0.10" before fetching. This is
  the productized form of the existing corpus reprobes.

### R9 — tooling: `spin`, a separate project tool

The compiler CLI stays what it is — gcc-like, one job: sources + `-I` roots
in, binary out, **no network, no manifest knowledge, no state** (the
compile-time counterpart of R2's no-code-at-fetch guarantee; hermetic builds
follow from the pair). Gem workflows live in a separate tool, **`spin`**
(mix/cargo-style), which owns everything stateful:

- `spin new` (scaffold `spinel-<name>/`), `spin add`/`remove` (edit manifest,
  re-resolve, update `gem.lock`), `spin lock`, `spin vendor`, `spin test`,
  `spin build`/`run` (resolve → assemble the `-I` root list and per-gem
  provenance from the lockfile/vendor tree → invoke `spinel`).
- The compiler never reads `gem.toml`: `spin` passes resolved roots (and the
  gem→root mapping R4's undeclared-dependency enforcement needs) on the
  command line. The zero-manifest escape hatch is therefore just "invoking
  `spinel -I` yourself".
- `spin` is written in Spinel and ships as a static binary — dogfooding both
  the language and, once bootstrapped, the gem format itself. Its own
  dependencies are stdlib-only (TOML parsing, subprocess git) to avoid a
  bootstrap cycle.
- Subcommands were considered on the compiler CLI and rejected: `spinel
  build` is ambiguous against `spinel build.rb` (a file), and the compiler
  binary should not carry TOML/git/network code.
- Known name collisions, accepted: Fermyon's wasm tool and the SPIN model
  checker also install `spin`; distro packages may need a `spinel-spin`
  package name even though the binary stays `spin`.

All offline-capable given a vendor tree; no daemon, no per-machine state
outside the XDG cache directory.

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
2. **Version skew between compiler and gems** — deferred, not merely open:
   the compiler itself carries no version number yet. The `spinel` manifest
   field stays reserved but unenforced; hard-vs-advisory is decided when
   toolchain versioning exists.
3. **Index coordination.** Concrete shape of seeding the Phase-2 index from
   the community spinelgems catalog (and who owns the name registry).

## 7. Decided

- Manifest is **TOML**, never executable (R2's no-code-at-fetch guarantee).
- Ecosystem name **spinelgems**; unit "gem"; one `gem.toml` for gems
  and applications alike (short over self-branding: inside a `spinel-*`
  checkout the context is clear, and the `spinel = "~> x.y"` constraint key
  doubles as the ecosystem discriminator should another toolchain ever
  adopt the same filename); lockfile `gem.lock` is an application artifact (libraries do not commit one,
  and a build without one resolves from the manifest).
- Project tool is **`spin`** — separate from the compiler CLI, written in
  Spinel, stdlib-only dependencies (R9); the compiler stays gcc-like and
  manifest-free.
- Gem *directories/repos* are `spinel-<name>` by convention; gem *names* (and
  therefore `require` strings) carry no prefix (R2). Gems-only directories:
  `gems/<name>/` in the compiler tree (pre-installed), `vendor/gems/
  <name>-<version>/` in a project, `~/.cache/spinel/gems/` for fetches;
  `lib/` reduces to runtime C only (R1).
- rubygems.org interop is **name policy only** (R5): same name means the same
  library (possibly a subset-compatible port); divergent forks rename.
  Publishing spinel gems on rubygems.org itself is out of scope.
