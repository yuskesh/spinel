# legacy

This directory holds the current Ruby implementation of Spinel.

- `spinel_analyze.rb`: self-hosted analyzer
- `spinel_codegen.rb`: self-hosted code generator
- `compiler_helpers.rb`: shared Ruby helpers for the compiler passes
- `node_table_loader.rb`: AST loader used by the Ruby backend
- `spinel-legacy` / `spinel-legacy.bat`: drivers for this backend
- `Makefile`: builds the backend + runs the self-host fixpoint

The active compiler migration work should happen in `../src/`.

This tree is a regression oracle only. `make legacy` / `make bootstrap`
build it entirely under `build/` (i.e. `legacy/build/`: binaries +
bootstrap intermediates); nothing here is installed. Drive it with the
`./spinel-legacy` script (it borrows the parser and runtime library from
the parent tree). The normal C build never touches this directory's
sources.
