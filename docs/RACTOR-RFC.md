# RFC: A Minimal Ractor for Spinel

Status: **experimental / Milestone 2** (working; ported to the C compiler in
`src/`). This document is both the design
rationale and the record of what currently ships. It proposes whether to
graduate to a full implementation (see *Path forward*).

## Why this is cheap here

Spinel's runtime (`lib/sp_runtime.h`) keeps essentially all mutable state in
file-scope `static` globals: the GC heap, the string heap, the dynamic symbol
table, the exception/`throw` `longjmp` stacks, the regex captures, the fiber
pointer, the lambda arena, the at-exit hooks, and the per-class object pools.
Generated code emits `$globals` and `@@cvars` as statics too. That is exactly
why Spinel is single-threaded.

Ractor's whole premise is **isolation + message passing**. Spinel's
"all state is a static global" shape maps onto that almost for free:

> Promote the per-execution-unit mutable globals to thread-local (`__thread`),
> run each Ractor as a pthread, and each Ractor gets its own private GC heap /
> string heap / symbol table / exception stack. Because each heap is private,
> **no global GC lock is needed** — Ractors collect independently. The
> compile-time-`const` class/dispatch tables stay shared and read-only.

A Ractor is, structurally, "a Fiber body run on a pthread, with mailbox/yield
queues instead of resume/yield slots, deep-copied at the boundary."

## The state partition

```
lib/sp_runtime.h globals, reclassified:

(a) __thread  — per-Ractor mutable, NO lock
    GC core (heap/bytes/threshold/old_heap/cycle/buckets), the GC root window
    (a lazily-malloc'd __thread pointer so it doesn't commit 512 KiB of TLS),
    the mark stack, the string heap + lcache, the exception/catch longjmp
    stacks, the regex captures ($~), the fiber root/current/list (both #ifdef
    branches), the lambda arena, the at_exit hooks, and the per-class pools.

(b) shared const, read-only — NO lock
    sp_class_names[] and all dispatch/vtable tables; the static portion of
    sp_sym_names[]; rodata string literals; install-once function-pointer
    hooks (sp_obj_hash_hook); sp_gc_verify.

(c) process-global WITH a lock — genuinely shared (all NEW infra)
    each Ractor's mailbox + outgoing queue (a mutex + condvar each) and the
    reference-counted control block. NOT the GC (private heaps), NOT malloc
    (glibc is already thread-safe).
```

`sp_thread_init()` wires each thread's `sp_fiber_current` to its own synthetic
root fiber (the address of a `__thread` var is not a constant expression, so it
can't be a static initializer). It runs on the main thread via an ELF
constructor and on each Ractor thread from its trampoline. The root array and
mark stack self-allocate lazily on first use.

## Architecture & data flow

```
Ractor.new { ... }  ──pthread_create──►  trampoline:
                                           sp_thread_init()  (fresh TLS heap + fiber root)
                                           set top-level setjmp landing pad
                                           run the block body (_ractor_body_N)
                                           close outbox; release ctrl refcount

parent: r.send(v) / r << v  ── serialize ─►  inbox  (mutex+condvar) ─► Ractor.receive
child:  Ractor.yield(v)     ── serialize ─►  outbox (mutex+condvar) ─► r.take : parent
```

**Boundary = serialize to a heap-neutral buffer.** A sent value is a pointer
into the sender's private heap; the sender's next GC would free it. Copying
directly into the receiver's heap is unavailable, because `sp_gc_alloc` writes
the *running* thread's thread-local GC slots — allocating into another heap
would need a per-heap lock on every send, reintroducing the global GC lock this
design eliminates. So the sender serializes into a malloc'd, pointer-free
buffer; the receiver rebuilds via ordinary `sp_gc_alloc` into its own heap; the
buffer is freed after. This matches CRuby's default deep-copy `send`.

## Public API (conforms to current CRuby Ractor)

```ruby
r = Ractor.new do
  x = Ractor.receive      # pop from this Ractor's inbox (also: Ractor.recv)
  Ractor.yield(x * 2)     # push to this Ractor's outbox
end
r.send(21)                # push to r's inbox (also: r << 21)
puts r.take               # => 42  (pop from r's outbox)
```

Default deep-copy semantics; capturing an unshareable outer variable is a
compile-time `Ractor::IsolationError`.

## Implementation map

(The active compiler is the C reimplementation under `src/`; the runtime is
split across `lib/sp_gc.c`, `lib/sp_fiber.c`, and the header `lib/sp_runtime.h`.)

- **`lib/sp_gc.{c,h}`, `lib/sp_fiber.{c,h}`, `lib/sp_runtime.h`** — `__thread` on
  every per-execution-unit mutable global (collector heap/roots/mark-stack/
  byte counters; fiber root/current/list; string heap + literal cache;
  `SP_POOL_DEFINE` pools; exception + catch stacks; regex match state; at_exit;
  lambda arena), plus `sp_fiber_thread_init()` (a main-thread constructor wires
  the main thread), `sp_gc_thread_teardown()` (frees a Ractor's heaps on exit),
  and the Milestone-2 message codec `sp_ractor_serialize`/`_deserialize`
  (installed into the runtime hooks by `sp_re_init`).
- **`lib/sp_ractor.{c,h}`** — `sp_Ractor` + mutex/condvar FIFO mailbox/outgoing
  queues, the pthread entry trampoline, and the serialize/deserialize hook
  plumbing. The generated body installs a top-level `setjmp` rescue pad so an
  uncaught Ractor exception terminates one Ractor rather than the process.
- **`src/analyze*.c`** — `TY_RACTOR`; `"Ractor"` as a builtin constant; inference
  arms (`Ractor.new → ractor`, `receive`/`take → poly`, `yield → nil`,
  `send`/`<< → ractor`).
- **`src/codegen*.c`** — `c_type TY_RACTOR → "sp_Ractor *"` and the scalar-return
  list; `emit_ractor_new` (Fiber-body-shaped, rejects captures/self with a
  compile-time `Ractor::IsolationError`); the class/instance dispatch arms.
- **`common.mk` / `src/main.c` / `Makefile`** — `-pthread` in `LDFLAGS`, in the
  in-process `cc` link driver, and on the new `sp_ractor.o` build rule.

## Deliberate divergences (where "minimal" buys simplicity)

1. **Value codec covers immediates, Symbol, String, and Arrays** (poly + typed
   int/float/string), deep-copied through a heap-neutral malloc'd blob
   (`sp_ractor_serialize`/`_deserialize`, installed via a hook because the
   rebuild allocators are static to the generated TU). Hashes, plain objects,
   Procs, Fibers, and IO raise `Ractor::Error` at the boundary — extending the
   codec to those (and to nested objects) is the main follow-up. Symbols travel
   by name and are re-interned in the receiver's per-Ractor table.
2. **FIFO mailbox / outgoing queue** (mutex + condvar, grows on demand), so a
   Ractor may `receive` several messages and a sender need not block per slot.
3. **`@@cvars` / `$globals` are `__thread`** → isolated per Ractor, rather than
   CRuby's "shared, raise on cross-Ractor mutation". Simpler and safe, but a
   real semantic difference: a class-body cvar initializer has not run in a
   child Ractor unless re-executed there. Programs that rely on cross-Ractor
   cvar/gvar sharing are out of scope for this milestone.
4. **`Ractor#send` with a bare String/Symbol *literal*** is parsed as a
   reflective send (`recv.send("m")` → `recv.m`, a whole-program parser
   behaviour), so send messages via `r << v` or a local (`r.send(v)`); literals
   like `r.send(42)` / `r.send([..])` are unaffected.
4. **No spawn args / block params** and **no shareable-by-value capture** yet:
   any captured outer variable or `self` is a compile-time
   `Ractor::IsolationError`. The block's return value is not delivered to
   `take` (only explicit `Ractor.yield` is).

## Verification

- The `__thread` storage-class conversion is behavior-neutral: the full
  existing test suite passes unchanged, single-threaded (896 pass / 0 fail /
  0 error with the two Ractor tests; CI green on ubuntu-gcc/clang + macOS-clang).
- `test/ractor_basic.rb` exercises `Ractor.new` / `receive` / `yield` /
  `send` / `<<` / `take` end-to-end and prints `42` twice; `test/ractor_messages.rb`
  round-trips String / Symbol / poly+typed Array messages.
- Clean under `SPINEL_GC_VERIFY=1`, **ThreadSanitizer** and **AddressSanitizer**
  (run binaries under `setarch -R` for TSan on this aarch64 box), and **valgrind
  memcheck + helgrind** report 0 leaks / 0 data races. Each Ractor's private GC
  and string heaps are freed on thread exit (`sp_gc_thread_teardown` +
  `sp_gc_str_teardown_hook`); the `sp_Ractor` control structs are retained in a
  process-lifetime registry (a detached worker plus a GC-untracked handle have
  no safe mid-run free point) so they stay reachable rather than leaking.
- The isolation rule rejects a block that captures a mutable outer local with
  `Ractor::IsolationError`.

## Path forward (if this graduates)

In rough dependency order: extend `sp_deep_copy` to strings → arrays → hashes →
objects (symbols travel by name, re-interned on receive); make the mailbox
unbounded; thread-local-ize `@@cvars`/`$globals` with a class-body initializer
replay on the child; add spawn args and shareable-by-value capture; and a
`Ractor::Port`-style multi-consumer surface. None of these change the core
partition above — they extend it.
