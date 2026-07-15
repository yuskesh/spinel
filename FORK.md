# A fork of spinel for the spinel-ebpf project

[matz/spinel](https://github.com/matz/spinel) is a whole-program type-inferring
Ruby AOT compiler that emits C. This fork exists to carry the small, env-gated
patch that the [spinel-ebpf](https://github.com/yuskesh/spinel-ebpf) project
depends on — and to be the base for upstreaming that patch.

It currently carries **two changes**, and they are different in kind:

| | Change | Kind |
|---|---|---|
| 1 | **`SPINEL_EXTERN_METHODS`** (`src/codegen.c`, +48 lines) | **Permanent.** Env-gated, additive. An upstream PR candidate. |
| 2 | **`return;` → `return 1;`** (`src/codegen_call.c`, 1 line) | **Temporary.** A fix for an upstream bug that breaks the gcc 14+ build. Dropped as soon as upstream fixes it. |

There are no other changes.

## Branches

- **`master`** — a clean mirror of `matz/spinel` master. Tracks upstream; no
  changes. (Sync it from upstream as usual.)
- **`c-emit-ir`** — `master` plus the two changes below. This is the branch
  spinel-ebpf builds against; its `scripts/setup.sh` defaults to a tag on this
  branch.

Tags of the form `spinel-ebpf-base-<date>` mark the exact (upstream + patch)
commit that a given spinel-ebpf revision was built against, for reproducible
setup.

## 1. The patch: `SPINEL_EXTERN_METHODS` (src/codegen.c, ~48 lines, additive)

When the `SPINEL_EXTERN_METHODS` environment variable lists method names, those
top-level methods are emitted as `extern` forward declarations — using spinel's
canonical value ABI (`mrb_int` params; `mrb_int` return for a concrete int,
otherwise `void`) — instead of full definitions. Their bodies are skipped, to be
provided by another translation unit linked into the program.

This enables separate compilation of selected methods: a hand-written C
implementation, an alternate runtime, or (in spinel-ebpf) forwarding a call to an
eBPF program so native code can invoke kernel-side methods transparently.

The hook is **generic** (nothing eBPF-specific) and **env-gated**: with
`SPINEL_EXTERN_METHODS` unset, this patch's codegen output is byte-identical to
upstream. It is a candidate for an upstream pull request to matz/spinel.

## 2. The temporary build fix (src/codegen_call.c, 1 line)

Upstream does not currently build with gcc 14 or newer. In `emit_class_new_call()`
— declared `static int` — one early-exit path returns no value:

```c
      buf_puts(b, ")))");
      return;        /* <- in a function returning int */
    }
```

gcc 14 treats `-Wreturn-mismatch` as a hard error, so the build fails. This fork
changes that line to `return 1;`.

This is a real bug, not a warning silencer. The caller branches on the return
value:

```c
  if (emit_class_new_call(c, id, b)) return;
```

Falling off the end returns an indeterminate value, so the "already handled"
test is unreliable and the call can be emitted twice. Every other handled path
in the same function returns `1`; this one line was the outlier.

Upstream CI does not catch it. `common.mk` sets `-Wno-all` in `CFLAGS`, and that
flag suppresses this diagnostic outright on the compilers CI actually runs. The
matrix is `ubuntu-latest / gcc`, `ubuntu-latest / clang` and `macos-latest /
clang`; `ubuntu-latest` is currently the ubuntu-24.04 image, whose default gcc is
13. gcc 14 is the outlier: it made `-Wreturn-mismatch` a permerror, which
`-Wno-all` cannot switch off.

Measured on upstream `7a216cc5`, compiling `src/codegen_call.c` with the exact
`CFLAGS` above:

| Compiler | Result |
|---|---|
| clang 19.1.7 | builds; no diagnostic at all |
| gcc 13.3.0 | builds; no diagnostic at all |
| gcc 14.2.0 | **fails**: `error: 'return' with no value, in function returning non-void [-Wreturn-mismatch]` |

So it is the flag, not the compiler family, that keeps CI green: drop `-Wno-all`
and clang 19 reports this same line as an *error* too. If GitHub's
`ubuntu-latest` moves to an image whose default gcc is 14 or newer, upstream's
own gcc job should begin failing here.

**This change is temporary and will be dropped as soon as upstream fixes the
line.** Unlike the patch above, it is not env-gated: it applies unconditionally.
It changes only the return value on a path upstream already intends to report as
"handled": wherever the indeterminate value already happened to be non-zero, the
generated output is unchanged; wherever it did not, upstream would have emitted
the call twice.

## Building

Same as upstream:

```sh
make deps && make
```

This produces `bin/spinel` plus the compiler objects (`build/csrc/*.o`) and
`build/libprism.a` that spinel-ebpf's in-process code generator links against.

## License

Inherited from upstream: MIT (© Yukihiro Matsumoto). The carried changes are
offered under the same terms.
