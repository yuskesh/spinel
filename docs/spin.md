# spin — the Spinel project tool (specification draft)

Status: specification draft for the tool decided in
[package-design.md](package-design.md) R9. Phase 1 scope (no registry)
unless marked Phase 2.

## 1. Principles

- `spin` owns every stateful operation: manifest editing, resolution,
  fetching, vendoring, invoking the compiler. The compiler (`spinel`) stays
  pure — sources + roots in, binary out, no network, no manifest knowledge.
- Every command works offline given a populated cache or vendor tree;
  network access happens only in commands documented as fetching.
- `spin` is written in Spinel and ships alongside the compiler. Toolchain
  versioning (and with it spin↔spinel skew handling and enforcement of the
  manifest's `spinel = "~> x.y"` constraint) is deferred: the compiler
  itself carries no version number yet. The manifest field stays reserved
  but unenforced.
- No daemon, no per-machine state outside `$XDG_CACHE_HOME/spinel/`.

## 2. Project model

- **Project root** = the nearest ancestor directory containing `gem.toml`
  (cargo-style upward walk from the working directory). All commands may run
  from any subdirectory.
- An **application** is a gem whose `bin/` is non-empty; `spin` needs no
  other distinction.
- **Build artifacts** split by whether they are application-specific.
  Whole-program compilation emits one type-specialized `.c` per target —
  different in every application, so per-project:

  ```
  build/
    bin/<target>       # final executables
    spin/<target>.c    # the generated whole-program C, kept for inspection
    test/<name>        # spin test binaries
  ```

  The single TU links directly, so a project build produces no `.o` of its
  own. `build/` is disposable (`spin clean`); `spin` never writes anywhere
  else in the project except `gem.toml` (`add`/`remove`), `gem.lock`
  (resolution) and `vendor/gems/` (`vendor`).
- Fetched gem sources live in the shared cache
  `$XDG_CACHE_HOME/spinel/gems/<name>-<version>/`, read-only after fetch.
  A gem's carried C is *not* specialized by inference (only Ruby is), so its
  `.o`/`.a` are project-independent and live beside the source cache in
  `$XDG_CACHE_HOME/spinel/native/<key>/`, keyed by (gem, version, toolchain,
  flags) per R6. Nothing is ever written into a gem's source tree — cache
  and vendor trees stay valid as read-only mounts.

## 3. Commands

Common flags: `--offline` (fail rather than touch the network), `--frozen`
(refuse if `gem.lock` is missing or does not satisfy `gem.toml` — CI mode),
`-q`/`-v`.

### spin new <name> / spin init

`new` scaffolds an **application** by default: a plain `<name>/` with an
identity-free manifest (a commented `[dependencies]` table — an application
is a gem with optional identity, so this is complete), `bin/<name>.rb`
printing hello, `test/`, and a `.gitignore` covering `build/`; then
`git init`. `--lib` scaffolds a library instead: `<name>.rb` at the root, no
`bin/`, a `[gem]` table with `name`/`version` filled in, and a README note
that the *published repo* is conventionally named `spinel-<name>`. `init`
writes only the manifest into the current directory (name derives from the
basename, a `spinel-` prefix stripped). Neither touches the network.

### spin add <name> [spec…] / spin remove <name>

Edits `[dependencies]` (or `[dev-dependencies]` with `--dev`), then
re-resolves and rewrites `gem.lock`. Source spec is one of:

```
spin add json                      # index (Phase 2) or already-cached name
spin add foo --git URL [--ref R]   # git source, ref defaults to the default branch
spin add foo --path ../spinel-foo  # local path (not locked to a hash; dev use)
spin add mini-assert --dev
```

`remove` drops the entry and re-resolves; it fails if another dependency
still requires the gem (the chain is printed).

### spin lock / spin fetch / spin vendor

- `lock`: (re-)resolve from `gem.toml` and write `gem.lock`. With
  `--update [name…]` move the named gems (default: all) forward to the
  newest versions the constraints admit — the only command that prefers
  newer versions (see §4).
- `fetch`: populate the cache with everything `gem.lock` (or a fresh
  resolution) names, without building. The only other network-touching
  commands are `add` and `lock`/`--update` when the needed sources are not
  cached.
- `vendor`: copy the resolved gem trees into `vendor/gems/`; subsequent
  builds prefer `vendor/` over the cache and work with `--offline`.
  Vendored trees are read-only inputs; `spin` never writes build artifacts
  into them.

### spin build [name…]

Resolves (lock-first; see §4), fetches missing sources (unless `--offline`),
then compiles. Targets: every `bin/<name>.rb` of the root gem, or the named
subset. `test/` is never a build target — tests are built and run only by
`spin test`. A gem with no `bin/` entries has nothing to build: `spin build`
errors and points at `spin test` (which is how a library gets compiled and
exercised).

- Per-target invocation: `spinel <entry> -I <gem-dir> … -I <extra>
  -o build/bin/<name>` — one `-I` per resolved gem, in resolution order (§5).
- `[build]` table in `gem.toml` may set `spinel-flags = ["--int-overflow=promote", …]`;
  `spin build -- <flags>` appends ad-hoc flags after `--`.
- Exit code is the compiler's on failure; diagnostics pass through verbatim
  (they already carry `gem/file.rb:line` positions) — with one addition:
  an unresolved-`require` error is wrapped with the command that would fix
  it (`hint: spin add <name> --git URL | --path DIR`), closing the add-a-gem
  loop without teaching the compiler about manifests.

### spin run [name] [-- args…]

`build` for the single named target (or the only target; ambiguity is an
error listing the candidates), then exec it with the arguments after `--`.

### spin test [file…] [--regen]

Per package-design R2 "Tests": each top-level `test/*.rb` of the root gem is
compiled as its own target (library sources + `[dependencies]` +
`[dev-dependencies]`) and run; pass/fail by `.expected` snapshot diff, else
by direct diff against `ruby` when available; `--regen` rewrites snapshots
from CRuby. File arguments restrict the set. Parallel by default
(`-j N` to bound).

### spin list / spin tree

`list` prints the resolved set (name, version, source, path); `tree` the
dependency graph with the requiring constraint on each edge. Both offline.

### spin install [name…]

Copies built executables to `~/.local/bin` (respecting `$XDG_BIN_HOME` /
`--prefix`), building first if needed — the last step of "I wrote a CLI and
now I use it". `--uninstall` removes them.

### spin clean

Removes `build/`. `--cache` additionally drops this project's entries from
the shared native-object cache (never fetched sources).

## 4. Resolution

**Minimal version selection** (Go-style MVS), proposed: among versions
admitted by all constraints, every gem resolves to the *lowest* admissible
version; `spin lock --update` is the explicit, only path that moves
forward.

- Deterministic without a lockfile — which is what makes `gem.lock`
  droppable for pinned-source projects (package-design R3) instead of
  load-bearing.
- No SAT solving; conflicts are reported as the two constraint chains that
  cannot meet, with the manifest lines that introduced them.
- The trade-off (not auto-riding the newest release) is intentional under
  whole-program inference, where a dependency bump can change whether an
  application *compiles*; upgrades are opt-in and diffable.

Order of authority: `gem.lock` if present and satisfying `gem.toml`
(otherwise error under `--frozen`, transparent re-resolve without);
`vendor/gems/` then cache for sources; `--path` deps always read live from
their path and are recorded in the lock without a hash.

One version per gem per application; the dependency graph must be acyclic;
feature-namespace overlap across the resolved set is a resolution error
(package-design R4).

UX principle: the lockfile is something you *diff*, never something you
read or edit — `spin add`/`lock` write it, `build`/`run`/`test` consume it,
and the only moments a developer meets it are `git add` and the diff of a
`spin lock --update`.

## 5. Compiler interface

`spin` talks to the compiler through the existing `-I` mechanism — one root
per resolved gem, appended in resolution order — plus additional options
only where `-I` genuinely cannot carry the information:

- Phase 1 passes plain `-I` roots and nothing else. The compiler stays
  entirely unaware of gems; `spin` is just a disciplined way of computing
  the `-I` list, and hand-written `spinel -I` invocations remain the
  zero-manifest escape hatch with identical semantics.
- The R4 undeclared-cross-gem-require enforcement needs per-root provenance
  the plain flag does not carry; when implemented it rides a supplementary
  option layered *on top of* `-I` (exact shape decided then), not a
  replacement for it. Until then R4's rule is checked by `spin` only at
  resolution granularity (the dependency must be in the resolved set).
- Carried C is discovered by extension (R2): every `.c` in a gem's tree
  (outside `build/`, `vendor/`, `test/`) is compiled by `spin` into the
  shared native cache and the objects handed to the compiler via repeatable
  `--link` flags (R6); `spinel` itself never compiles gem C. Objects are
  keyed `<gem>-<version>-<cc>` and rebuilt when any of the gem's `.c`/`.h`
  is newer; the gem's own tree and the compiler's runtime headers are on
  the include path. External libraries stay on the existing `ffi_lib`
  Ruby-side DSL (its SPINEL_LINK markers already reach the link line).

### Rebuilds & staleness

`spin` is the build system: a normal application needs no Makefile. Under
whole-program compilation a target's build inputs are a *flat set* — every
resolved `.rb`/`.rbs`, the toolchain, the flags, the runtime — so there is
no per-file dependency graph to solve: `spin build` content-hashes the input
set per target and either skips (unchanged) or recompiles the whole target
(the compiler-repo content-stamp scheme). File-granular incremental
compilation intentionally does not exist — type specialization spans every
source — and is compensated by compiler speed, the shared native cache
(gem C rebuilt only when its (gem, version, toolchain, flags) key changes),
and per-target parallelism across bins and tests. `spin` runs no arbitrary
build tasks (no rake surface, same spirit as the non-executable manifest);
projects with bespoke build steps invert the relationship and call `spin` —
or `spinel` directly — from their own build system.

## 6. Output & errors

- Quiet by default: one line per phase (`resolve`/`fetch`/`build target`),
  wall-clock on completion. `-q` errors-only, `-v` full compiler command
  lines.
- Machine-readable: `spin list --json`, `spin tree --json`.
- Exit codes: 0 ok; 1 build/test failure; 2 resolution failure; 3 usage.

## 7. Prerequisites & phasing

Phase 1 needs, beyond `spin` itself:

1. **`toml` stdlib feature** — `spin` is stdlib-only Spinel; a TOML parser
   joins the require-gated stdlib (and immediately dogfoods R8's carve-out).
2. The `gems/` carve-out of the bundled stdlib (package-design R1), so
   resolution order native → stdlib → gems → `-I` is observable.

No compiler changes are required for Phase 1 (§5: plain `-I`).

Phase 2 adds: index-backed `spin add name`, `spin search`, `spin publish`
(index PR automation), and probe-result surfacing in `add`/`build` warnings
(R8).

## 8. Open points

- Whether `--update` should support constraint-raising edits
  (`spin add json@2` style) or leave manifest edits manual.
- Native-cache eviction policy (currently: never; `spin clean --cache`).
- The provenance option layered on `-I` for R4 enforcement (§5), once
  cross-gem require checking moves into the compiler.
- Toolchain versioning and the `spinel` manifest constraint (§1), once the
  compiler carries a version.

## 9. gem.lock (M1)

TOML, one table per resolved gem; readable by the same minimal reader:

```toml
[lock.ansi]
version = "0.1.0"        # from the gem's own gem.toml at the pinned ref
git = "https://…"        # exactly one of git / path
ref = "<commit SHA>"     # always a full SHA, never a branch name
path = "../spinel-ansi"  # path deps are recorded but never pinned
```

M1 resolution is **verification, not selection**: with no index, a git ref
or a path is the only candidate, so `spin lock` walks the dependency graph
(a fetched gem's own `[dependencies]` may carry git/path sources;
transitive cycles are an error), records the SHA each source resolves to,
and checks declared version constraints against the versions found. MVS
selection activates in Phase 2 when an index offers version sets. Content
hashes ride on the git SHA in M1; tree hashing arrives with the index.
