# spin — projects and packages

`spin` is Spinel's project tool: it scaffolds a project, resolves
dependencies (spinelgems), and drives the compiler so you never write a
Makefile or a `spinel -I ...` line by hand. If you know cargo or mix, you
know the shape. The design record lives in
[internals/spin.md](internals/spin.md); this page is how to use it.

`spin` ships beside the compiler: building the repo (`make`) produces
`bin/spin`, and `make install` installs it next to `spinel`.

## Starting a project

```sh
spin new myapp        # scaffold: gem.toml, bin/myapp.rb, test/, .gitignore
cd myapp
spin run              # compile bin/myapp.rb and run it
```

```
myapp/
  gem.toml            # the manifest (name, [dependencies])
  myapp.rb            # library code: require "myapp" resolves here
  myapp/              # subfeatures: require "myapp/util" -> myapp/util.rb
  bin/myapp.rb        # each bin/*.rb is an executable (a compile root)
  test/               # each test/*.rb is a test program
  build/              # disposable output (spin clean)
```

An application **is** a gem: there is no separate project kind. Executables
live in `bin/` (one per file, `spin run <name>` when there are several), and
a gem consumed as a library simply has no `bin/`. `spin init` writes a
`gem.toml` into an existing directory instead of scaffolding.

Everything in the gem participates by extension, not by manifest lists:
`.rb` is source, `.rbs` is an optional type sidecar, `.c`/`.h` is carried
native code (below). `build/`, `vendor/`, `test/`, and `bin/` are the only
special directory names.

## Dependencies

Declare dependencies in `gem.toml`; `spin` computes the compiler's `-I`
list from them. Three source forms:

```toml
[dependencies]
ansi  = { path = "../spinel-ansi" }              # local checkout
greet = { git = "https://github.com/x/spinel-greet" }  # git URL (+ ref = "...")
hello = "~> 1.1"                                 # index constraint (see below)
```

```sh
spin add ansi --path ../spinel-ansi   # edits gem.toml and relocks
spin add hello --version "~> 1.1"     # index form
spin remove ansi
spin list                             # resolved set: name, version, source
spin tree                             # nested view (--json on both)
```

Dependencies are transitive: each fetched gem's own `[dependencies]` is
resolved too. Inside a project every `require` must resolve — an
unsatisfiable `require` is a compile error naming the missing gem, and
stdlib features need their `require` just like CRuby (`spin` compiles with
the require gate on; see [require.md](require.md)).

### The index

A bare `name = "constraint"` dependency is looked up in the index — a git
repository (no server) mapping names to repos and releases:
<https://github.com/matz/spinel-index>. Constraints are `"~> 1.2"`
(pessimistic), `">= 1.2.3"`, an exact version, or `"*"`.

Selection is MVS: `spin` picks the **lowest** release satisfying the
constraint, so a build without a lockfile is still deterministic;
`gem.lock` then pins the exact commit. `spin search [term]` lists index
entries. Set `SPIN_INDEX` to use another index (a `file://` URL works).

### gem.lock

`spin lock` (and `spin add`/`remove`) writes `gem.lock`: one `[lock.<name>]`
entry per dependency with the resolved version and, for git/index sources,
the full commit SHA. Commit it for applications. Resolution *verifies*
against the lock rather than reselecting; if you change a constraint so the
pinned version no longer satisfies it, the build warns and reselects, and
the next `spin lock` rewrites the pin.

### Offline and vendoring

Fetched gems live in a shared cache (`$XDG_CACHE_HOME/spinel/gems/`),
keyed by the commit SHA. `spin vendor` copies the resolved tree into
`vendor/gems/` for hermetic builds; with `SPIN_OFFLINE=1`, resolution uses
only the cache and `vendor/` — nothing touches the network.

## Tests

Each `test/*.rb` is one test program, compiled with the gem's sources and
dependencies spliced in. Pass/fail is snapshot-based:

```sh
spin test                 # run all tests
spin test smoke_test.rb   # one test
spin test --regen         # refresh .expected snapshots from CRuby
```

A committed `test/<name>.rb.expected` is diffed against the run's stdout.
With no snapshot, the same file runs under `ruby` and the outputs are
diffed directly — the test doubles as a CRuby-parity check. A non-zero
exit or a diff fails, so plain assert-and-raise style works.

## Native C in a gem

Drop `.c`/`.h` files anywhere in the gem tree and bind them with the
[FFI declarations](FFI.md):

```ruby
# fast.rb
module Fast
  ffi_func :fast_quad, [:int], :int
end
```

```c
/* fast_ext.c */
#include <stdint.h>
intptr_t fast_quad(intptr_t x) { return x * 4; }
```

`spin` compiles each `.c` once into a shared cache keyed by
(gem, version, toolchain) — set `CC` to choose the compiler — and links the
objects into every dependent build. External libraries use the existing
`ffi_lib` declaration and need no manifest entry.

## Rebuilds

`spin build`/`run`/`test` skip recompilation when nothing changed (input
mtimes across the project, its dependencies, and the compiler binary).
There is no file-granular incremental mode — whole-program type
specialization spans every source — but compiles are fast and gem C
objects are reused from the cache. `spin clean` removes `build/`.

## Command summary

| command | what it does |
|---|---|
| `spin new <name> [--lib]` / `spin init` | scaffold / adopt a directory |
| `spin build [target..]` | compile `bin/` executables into `build/bin/` |
| `spin run [target] [-- args]` | build, then run one executable |
| `spin test [file..] [--regen]` | run `test/*.rb` against snapshots |
| `spin add` / `remove` | edit `[dependencies]` and relock |
| `spin lock` / `fetch` / `vendor` | pin / warm the cache / copy into `vendor/` |
| `spin list` / `tree` / `search` (`--json`) | inspect the resolved set / the index |
| `spin clean` | remove `build/` |

Environment: `SPIN_INDEX` (index URL), `SPIN_OFFLINE=1` (cache/vendor
only), `CC` (toolchain for gem C).
