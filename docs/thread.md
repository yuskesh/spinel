# `Thread` in Spinel

Spinel runs Ruby `Thread`s as **true parallelism with no GVL**. A threaded
program is scheduled onto `N` OS workers that run green threads over a
stop-the-world garbage collector, so CPU-bound threads make progress on
separate cores at the same time. This is the JRuby / TruffleRuby model, not
CRuby's: individual operations are **not** made atomic by a global lock.

This document describes what you can rely on when you use threads, which of the
`Thread` API is supported, and the semantics that differ from CRuby. The
implementation (the M:N scheduler, per-worker run queues, work stealing, the
preemption monitor) is a separate concern and lives in
[internals/thread-mn-design.md](internals/thread-mn-design.md) — nothing there
is a user guarantee.

## The execution model

- **N OS workers.** Worker count is `min(online cores, SPINEL_WORKERS)`.
  Set `SPINEL_WORKERS=1` to force the single-worker cooperative model, or a
  fixed number to cap parallelism. Absent the env var, Spinel autodetects the
  number of online cores.
- **Real parallelism, no GVL.** Two threads run Ruby code on two cores
  simultaneously. There is no implicit per-object lock, so unsynchronized
  shared mutation is a data race (see [By design](#by-design) below).
- **Green threads over a stop-the-world GC.** A `Thread` is a green thread; the
  workers multiplex many green threads onto few OS threads. A garbage
  collection stops all workers at a safepoint, so allocation stays correct
  under parallelism without per-object write barriers.
- **Preemption.** A monitor thread timeslices CPU-bound threads (~10 ms
  quantum) so a thread that loops without yielding cannot starve its siblings.
  Preemption is taken at **safepoint polls** (loop back-edges): a thread that
  spends a long time inside a single runtime call with no poll yields only when
  that call returns. The preemption signal is `SIGURG`, overridable with
  `SPINEL_PREEMPT_SIGNAL`.
- **The single-threaded archive is unchanged.** A program that never uses
  `Thread` compiles and runs byte-identically to before; none of the scheduler
  machinery is linked in.

## Supported API

### `Thread`

| method | notes |
|---|---|
| `Thread.new(*args) { \|*args\| ... }` | spawn; args become the block parameters |
| `#join` | block until finished, re-raising an unhandled exception in the caller |
| `#value` | `#join` plus the block's result |
| `#kill` / `#exit` / `#terminate` | terminate the thread |
| `#raise(...)` | raise an exception in the thread |
| `Thread.pass` | cooperative yield |
| `Thread.current` / `Thread.main` | the running / initial thread |
| `Thread.list` | live threads |
| `#alive?` / `#status` | `"run"` / `"sleep"` / `false` / `nil` |
| `#name` / `#name=` | a string or nil |
| `Thread#[]` / `#[]=` / `#key?` | fiber-local / thread-local storage by symbol |
| `Thread.report_on_exception` / `=` and the per-thread accessors | unhandled-exception reporting |

`Kernel#sleep` and blocking I/O are **scheduler-aware**: a sleeping or
I/O-blocked thread frees its OS worker for other green threads instead of
holding it idle, and the monitor wakes it when the deadline passes or the fd
becomes ready.

### Synchronization primitives

Real, blocking primitives — not busy-waits:

| type | methods |
|---|---|
| `Mutex` | `#lock` / `#unlock` / `#try_lock` / `#locked?` / `#owned?` (non-recursive) |
| `Queue` | `#push` / `#<<` / `#pop` / `#size` / `#empty?` / `#close` / `#closed?` / `#clear` (`#pop` blocks when empty) |
| `SizedQueue` | `Queue` plus `#max`; `#push` blocks when full |
| `ConditionVariable` | `#wait(mutex)` / `#signal` / `#broadcast` |

## By design

These are deliberate consequences of real parallelism, listed in
[limitations.md](limitations.md#by-design-deliberate-choices):

- **Data races are observable.** Two threads mutating the same
  `Array`/`Hash`/object without a `Mutex` is undefined at the Ruby level,
  exactly as in JRuby and TruffleRuby. CRuby's GVL makes individual operations
  appear atomic; Spinel does not, and adds no implicit per-object locking.
  Correctness across threads is the program's responsibility via
  `Mutex` / `Queue` / `ConditionVariable`.
- **Interleaving is nondeterministic.** The ordering of `Thread.pass`,
  `Thread.list` membership, and the exact moment a `Thread#raise` / `#kill` is
  delivered are nondeterministic, where the single-worker model was
  deterministic.

## Environment variables

| variable | effect |
|---|---|
| `SPINEL_WORKERS` | number of OS workers; overrides the online-core autodetect (min 1) |
| `SPINEL_PREEMPT_SIGNAL` | the signal the monitor uses to preempt a busy worker (default `SIGURG`) |
