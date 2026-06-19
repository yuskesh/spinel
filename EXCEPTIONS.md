# Exception handling design notes

Spinel emits C and currently implements Ruby exceptions with `setjmp`/`longjmp`
(`sp_exc_stack[]` / `sp_exc_top` in `lib/sp_runtime.h`; `begin/rescue/ensure/
retry` lowered in codegen). This document records the design goals, the
alternatives considered, and the measurements that settled the question, so the
analysis is not re-derived each time.

## Goals (matz)

- Cost when **no** exception is thrown must be **zero or very low**.
- Cost **when** an exception is thrown does not matter.
- `setjmp`/`longjmp` should be avoided if practical.
- Generated C verbosity does not matter (it is machine-generated).
- Returning a struct (value + error channel) is acceptable.
- Sequential per-call checking is a cost concern.
- **Microcontroller (low-memory) execution is a core motivation and cannot be dropped.**

## The fundamental C constraint

A cross-frame non-local jump in portable C requires **either** `setjmp`/`longjmp`
**or** the platform unwinder (`_Unwind_*`). Avoiding both forces unwinding the
stack one frame at a time via **return-value propagation** — i.e. a check after
every call that may raise. So "zero happy-path instructions" is only reachable
with table-based unwinding (C++/Itanium EH).

## Mechanisms by happy-path cost

| Mechanism | Examples | Happy path | Unwind | Fits a C emitter |
|---|---|---|---|---|
| setjmp/longjmp | Lua, mruby, CRuby, current Spinel | volatile residual | med | yes (current) |
| Table-based EH | C++, Rust panic, Swift, Crystal | "zero" instructions, but see measurement | heavy | only via C++ output |
| error-in-register / error union | Swift `throws`, Zig `!T`, Herbceptions | one predictable branch per throwing call | light | codegen change, per-call cost |
| Return-value flag | Go err, CPython, Nim `--exceptions:goto` | per-call check | light | codegen change |
| C++ try/catch (looks-C via macros) | Nim C++ backend, Haxe | see measurement | heavy | C++ toolchain |

## Microcontroller reality

C++ **exceptions** are almost always disabled on MCUs (`-fno-exceptions`):
unwind tables consume flash, the unwinder adds footprint, `throw` allocates and
has non-deterministic timing. arm-none-eabi / avr-gcc / ESP-IDF / Zephyr all
default exceptions off; MISRA/AUTOSAR restrict them. So **C++ EH is unavailable
on the MCU target**, and `setjmp` (no unwind tables, tiny `jmp_buf`) is the
right low-memory mechanism there — return-value would add code at every call
site, which is *worse* for flash. This is why mruby/Lua use setjmp on embedded.

A macro abstraction (`SP_TRY` / `SP_RESCUE` / `SP_RAISE`) can switch cleanly
between **two boundary-only mechanisms** (C++ EH on host, setjmp on MCU) because
neither needs per-intermediate-call code. It **cannot** abstract return-value
(that needs codegen to emit per-call propagation, not just macro expansion).

## Measurements

### Prior (2026-06-07): return-value rejected

A global `sp_err` flag + `if (sp_err) goto` after every statement was
prototyped and measured: optcarrot **753 -> 527 fps (~-30%)**. optcarrot is
method-call-dense = worst case for per-call checks. Rejected.

### 2026-06-18: C++ EH measured (this session)

optcarrot's generated C was compiled and run as C++ (checksum 59662 preserved).

Feasibility — C++ compatibility is **not** a real barrier:
- 33 g++ errors total; **~73% are `volatile sp_RbVal`** copy errors, i.e. caused
  by the setjmp `volatile` machinery that a C++-EH (no-setjmp) design removes.
- The rest: an anonymous `enum` inside `struct sp_Val` needs hoisting to file
  scope (C injects its enumerators into the enclosing scope, C++ does not;
  1 line), 2 implicit `void*` conversions (explicit cast / `-fpermissive`), and
  archive symbols need `extern "C"` (C++ would mangle the calls otherwise).
- No designated initializers in `sp_runtime.h` (the worst C/C++ landmine is absent).
- 1 C++-keyword identifier collision (`this`) in optcarrot — mangle to fix.

Runtime cost (10 runs, max fps; same source, only the EH flag differs):

| build | fps | vs no-EH ceiling |
|---|---|---|
| g++ `-fno-exceptions` (volatile stripped) | **736** | — (ceiling) |
| current setjmp (gcc, volatile present) | **716** | -2.7% (volatile/setjmp residual) |
| g++ C++ EH enabled, no `noexcept` | **689** | **-6.4%** |

Key finding: **C++ EH is not zero-cost here.** With *zero* actual `throw`s,
enabling `-fexceptions` costs ~6.4% purely from optimization inhibition (g++
treats every non-`noexcept` call as may-throw and maintains unwind state).
optcarrot is call-dense and GC'd (no destructors), yet the tax is large. As a
result **C++ EH (689) is slower than the current setjmp build (716)**: the
EH-enable tax (6.4%) exceeds the volatile tax (2.7%) it would remove.

Caveat — `noexcept` ceiling: 689 is the no-annotation worst case. `-fno-exceptions`
(= effectively all-`noexcept`) is 736, so annotating provably-non-raising
functions `noexcept` could approach 736 and beat setjmp by ~3%. But that needs
the same can-raise analysis as the return-value approach, the genuinely-raising
functions still pay, and the realistic landing is between 689 and 736.

## Conclusion (2026-06-18)

- **Keep `setjmp`/`longjmp`.** Re-confirmed by the new C++ EH data: EH's ceiling
  is only ~setjmp+3% (needs full `noexcept` analysis) and its floor is below
  setjmp; return-value was -30%. No mechanism beats setjmp clearly on the
  happy path for this call-dense workload, and setjmp is also the correct
  low-memory choice for the MCU target.
- **The real, bounded win is the volatile residual (~2.7%)**, taken without
  replacing the mechanism — narrow `volatile` to values that actually live
  across a `begin/rescue/retry` boundary (see SETJMP.md), not whole functions.
- C++ EH (looks-C via macros) stays feasible and is the only route to true
  zero-instruction happy path, but only if a future `noexcept`-via-can-raise
  experiment shows it reaching ~736; out of the box it regresses. Parked.
- Related latent correctness items on the setjmp path: #773 (volatile pointer
  locals), #1474 (fiber x exception cross-stack longjmp).

## Reproduce

```sh
make optcarrot                                   # regenerates build/optcarrot-single.c (WRAP)
sed 's/\bvolatile //g' build/optcarrot-single.c > /tmp/oc.cpp
# patch a copy of lib/sp_runtime.h: hoist `enum {SP_PROC2,...}` out of struct sp_Val;
# wrap the header body in extern "C" { ... }
g++ -std=c++17 -O2 -fpermissive -I/tmp/cpplib -Ilib/regexp -DSP_INT_OVERFLOW_MODE_WRAP \
    -c /tmp/oc.cpp -o /tmp/oc.o                  # EH on
g++ ... -fno-exceptions ...                      # EH off (ceiling)
g++ -O2 /tmp/oc.o lib/libspinel_rt.a -lm -Wl,--gc-sections -o /tmp/oc_cpp_eh
```
