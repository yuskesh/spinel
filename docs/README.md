# Spinel documentation

User documentation for Spinel, the whole-program ahead-of-time Ruby→C compiler.
Start here, then open the topic you need.

## For users

- **[limitations.md](limitations.md)** — what an AOT compiler can and cannot do.
  The honest catalogue of where Spinel differs from CRuby, and why. Read this
  first if something behaves unexpectedly.
- **[REQUIRE.md](REQUIRE.md)** — how `require` works: which stdlib needs which
  `require`, what an absent or unsatisfiable `require` does, and how to provide
  a feature of your own with `-I`.
- **[FFI.md](FFI.md)** — call C functions directly from Spinel Ruby, with no
  extension build step: declarations in the source become direct C call sites.
- **[RBS-EXTRACT.md](RBS-EXTRACT.md)** — seed the type inferencer with `.rbs`
  signatures via `spinel --rbs DIR`, and the supported RBS subset.
- **[FLOAT-ROUNDING.md](FLOAT-ROUNDING.md)** — the return type of
  `Float#ceil`/`#floor`/`#round`/`#truncate`, where Spinel's static typing meets
  CRuby's value-dependent rule.

## Internals

How the compiler is built and where it is going. Not needed to *use* Spinel.

- **[internals/AST.md](internals/AST.md)** — the text AST the parser emits and
  the rest of the compiler consumes.
- **[internals/ANALYZE-IR.md](internals/ANALYZE-IR.md)** — the analyze ↔ codegen
  contract (the shared in-memory `Compiler` model).
- **internals/*-design.md / *-plan.md** — design notes and roadmaps for
  in-progress work (require-gate, M:N threads, promote-mode). Working documents,
  not user guarantees.
