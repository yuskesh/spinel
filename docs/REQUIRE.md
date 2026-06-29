# `require` in Spinel

Spinel is a *subset* of Ruby: a program that compiles and runs under Spinel
should behave the same under CRuby. One place Spinel used to be a *superset*
instead was `require`. Some stdlib that CRuby gates behind a `require` —
`StringIO`, `IO#winsize`, and friends — was always available in Spinel, so code
that forgot the `require` ran under Spinel but raised `NameError` /
`NoMethodError` under CRuby. The **require-gate** closes that gap.

This document describes how `require` works today, which stdlib needs which
`require`, and how features are resolved. For the design rationale and the
planned package system see [require-gate-design.md](require-gate-design.md).

## `require` vs `require_relative`

They are different mechanisms and stay distinct:

| form | resolves to | when |
|---|---|---|
| `require_relative "path"` | a project-local file, spliced into the program | always; a missing file is a **compile error** |
| `require "name"` | a named **feature** (bundled stdlib, native, or — planned — a package) | the feature must exist, else a **compile error** |

`require` is *not* a runtime file load. Spinel is a whole-program
ahead-of-time compiler, so every `require` is known at compile time and there is
no `$LOAD_PATH` to mutate at runtime. A `require` that names something Spinel
cannot provide is reported when you compile, not when you run.

## The require-gate

The require-gate makes require-gated stdlib unavailable unless you `require` it,
matching CRuby. It is currently **opt-in**: set the environment variable
`SPINEL_REQUIRE_GATE=1` when compiling. With it off (the default today) the old
always-available behaviour is preserved. The gate is expected to become the
default at a future release boundary.

```sh
SPINEL_REQUIRE_GATE=1 spinel myprogram.rb
```

### Require-gated stdlib

These are provided by Spinel but, like CRuby, only after their `require`:

| `require` | what it enables | without the require (gate on) |
|---|---|---|
| `require "stringio"` | `StringIO` | uninitialized constant |
| `require "strscan"` | `StringScanner` | uninitialized constant |
| `require "json"` | `JSON.generate`, `JSON.dump` | uninitialized constant |
| `require "monitor"` | `Monitor` (`#synchronize`) | `NameError` (uninitialized constant) |
| `require "io/console"` | `IO#winsize` | `NoMethodError` |
| `require "time"` | `Time#iso8601` | `NoMethodError` |

Two shapes, which set what the failure looks like:

- A feature that **defines a class/module** (`stringio`, `strscan`, `json`,
  `monitor`): without the `require` the constant is undefined.
- A feature that **extends a core class** (`io/console` adds `IO#winsize`,
  `time` adds `Time#iso8601`): without the `require` the method is undefined.
  The rest of `IO` and `Time` are core and always available — only the gated
  method needs the `require`.

```ruby
# gate on, no require:
StringIO.new("x")     # error: uninitialized constant StringIO
STDOUT.winsize        # error: undefined method 'winsize'

# correct:
require "stringio"
require "io/console"
StringIO.new("x").read
STDOUT.winsize
```

### Unsatisfiable requires

A `require` that Spinel cannot satisfy at all — a stdlib Spinel does not
implement (`date`, `pp`, `securerandom`, ...) or an unknown name — is a compile
error under the gate, the same as a missing `require_relative`:

```
spinel: cannot load such file -- date (require "date")
```

This is honest about the subset: if Spinel cannot provide `date`, the program
genuinely cannot run, and you learn it when you compile rather than via a
confusing failure later. With the gate off this is a warning instead.

A few `require`s name a capability Spinel already provides as core, and are
**tolerated** as a no-op (modern CRuby treats them as already-loaded too):

- `require "thread"` (Thread, Mutex, Queue are core)
- `require "enumerator"` (Enumerator is core)
- `require "fiber"` (Fiber is core)

### Bundled pure-Ruby stdlib

Some stdlib ships with Spinel as Ruby source under `lib/` and is spliced when
required — `set`, `forwardable`, `optparse`, `erb`. These already behaved
correctly (no `require`, no definition), so the gate does not change them; the
`require` pulls in the file.

## Providing your own feature

You can provide a feature yourself and have `require "name"` resolve it, through
the same mechanism as bundled stdlib. Pass `-I <dir>` (like `ruby -I`) to add a
feature search root, then a `require` resolves against it:

```sh
spinel -I mylibs main.rb
```

A feature name is a path, looked up in each `-I` root in two forms:

- **single file** — `require "thing"` → `mylibs/thing.rb` (the CRuby form);
- **colocated directory** — `require "thing"` → `mylibs/thing/thing.rb`, so a
  feature's sources (`.rb`, later its `.c`/`.rbs`) share one directory.
  `require "my/thing"` → `mylibs/my/thing.rb` or `mylibs/my/thing/thing.rb`.

Pure Ruby needs only the `.rb`: it is spliced into the whole-program compile and
Spinel infers types from it like your own code, so no manifest or `.rbs` is
required (an `.rbs` is optional, to pin the public surface). A `require` that no
root satisfies is the compile error from the previous section.

### Planned

- C in a feature is reached through [FFI](FFI.md) today (wrapping an existing
  library); a planned in-TU mode will let a feature's own C be inlined without a
  link boundary.
- A default vendored root and a project manifest will sit on top of `-I`, so
  packages resolve without passing `-I` by hand.
- Spinel's own require-gated stdlib will eventually be carved out of the
  compiler into ordinary feature packages, resolved exactly like a third-party
  one.

See [require-gate-design.md](require-gate-design.md) for the full design.
