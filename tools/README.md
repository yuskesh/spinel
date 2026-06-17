# spinel tools

Developer tools that ship with the compiler. They are written in the
spinel subset and compiled by `spinel` itself, so their only runtime
dependency is `cc` — the same as the compiler. `make` builds them
(`build/spinel-<name>`) and `make install` places them on `PATH` beside
`spinel`, so each is invoked like a subcommand:

```
spinel-doctor app.rb
spinel-reduce app.rb
spinel-flatten app.rb
```

They locate the compiler at run time via, in order: `$SPINEL` (an
explicit path), `$SPINEL_DIR/spinel`, then `spinel` on `PATH`.

## spinel-doctor

One health report for a program. Independent legs:

- **build** — compiles to a binary; reports any compile / codegen /
  C-build failure (with `--line-map`, so C errors point at Ruby lines).
- **unsupported** — codegen gaps that degrade to a stub.
- **unresolved** — calls that silently degrade to `nil`/`0` where CRuby
  would raise (via `SPINEL_WARN_UNRESOLVED`).
- **requires** — non-relative `require`s spinel treats as native / no-op.
- **behavior** — optional: compiled output vs CRuby; needs `ruby` on
  `PATH` and skips cleanly otherwise.

```
spinel-doctor [--only a,b] [--skip a,b] [--behavior] [--quiet] app.rb
```

Exit `0` clean, `1` when any leg reports an error-severity finding.

## spinel-reduce

Delta-debug (ddmin) a degrading program down to a minimal input that
still reproduces a chosen failure. Re-runs `spinel` only.

```
spinel-reduce [--oracle build|unsupported|unresolved] \
              [--oracle-cmd 'CMD {}'] [-o OUT] app.rb
```

`--oracle` selects the built-in interestingness test (default `build` =
spinel exits non-zero). `--oracle-cmd` is an escape hatch: `{}` is
replaced by the candidate file and the candidate is kept when `CMD`
exits `0`. Flatten multi-file programs first so reduce has one input.

## spinel-flatten

Inline a `require_relative` graph into one self-contained file, so
`spinel-reduce` and bug reports operate on a single input.

```
spinel-flatten [-o OUT] app.rb
```

## Adding a tool

Drop `tools/<name>.rb` (subset Ruby, `require_relative "tool_common"`
for the shared helpers); `make` compiles it to `build/spinel-<name>` and
`make install` installs it. Keep it within the subset — a tool that
stops compiling breaks the build.
