# A fork of spinel for the spinel-ebpf project

[matz/spinel](https://github.com/matz/spinel) is a whole-program type-inferring
Ruby AOT compiler that emits C. **This fork exists only to carry one small,
env-gated patch** that the [spinel-ebpf](https://github.com/yuskesh/spinel-ebpf)
project depends on — and to be the base for upstreaming that patch.

There are no other changes. With the patch's environment variable unset, the
compiler is byte-identical to upstream.

## Branches

- **`master`** — a clean mirror of `matz/spinel` master. Tracks upstream; no
  changes. (Sync it from upstream as usual.)
- **`c-emit-ir`** — `master` plus the single patch below. This is the branch
  spinel-ebpf builds against; its `scripts/setup.sh` defaults to a tag on this
  branch.

Tags of the form `spinel-ebpf-base-<date>` mark the exact (upstream + patch)
commit that a given spinel-ebpf revision was built against, for reproducible
setup.

## The patch: `SPINEL_EXTERN_METHODS` (src/codegen.c, ~48 lines, additive)

When the `SPINEL_EXTERN_METHODS` environment variable lists method names, those
top-level methods are emitted as `extern` forward declarations — using spinel's
canonical value ABI (`mrb_int` params; `mrb_int` return for a concrete int,
otherwise `void`) — instead of full definitions. Their bodies are skipped, to be
provided by another translation unit linked into the program.

This enables separate compilation of selected methods: a hand-written C
implementation, an alternate runtime, or (in spinel-ebpf) forwarding a call to an
eBPF program so native code can invoke kernel-side methods transparently.

The hook is **generic** (nothing eBPF-specific) and **env-gated**: with
`SPINEL_EXTERN_METHODS` unset, codegen output is byte-identical to upstream. It is
a candidate for an upstream pull request to matz/spinel.

## Building

Same as upstream:

```sh
make deps && make
```

This produces `bin/spinel` plus the compiler objects (`build/csrc/*.o`) and
`build/libprism.a` that spinel-ebpf's in-process code generator links against.

## License

Inherited from upstream: MIT (© Yukihiro Matsumoto). The carried patch is offered
under the same terms.
