# Integer overflow (`--int-overflow`)

CRuby's `Integer` is arbitrary precision: it never overflows, it grows. Spinel
compiles `Integer` to a fixed-width **64-bit** machine integer (`mrb_int`,
range `-2**63 .. 2**63 - 1`) because a machine word is what makes the generated
C fast. `--int-overflow=MODE` chooses what happens when an `Integer` result
crosses that 64-bit boundary.

```sh
spinel app.rb --int-overflow=raise     # default
spinel app.rb --int-overflow=wrap
spinel app.rb --int-overflow=promote
```

## Modes

| mode | on overflow | matches CRuby? | use it for |
|---|---|---|---|
| **`raise`** (default) | raises `RangeError` (`integer overflow in +`) | no — CRuby would grow the integer | catching overflow loudly; never silently wrong |
| **`wrap`** | two's-complement wraparound, like C (`a + b` with no check) | no | modular arithmetic, hashes, checksums, PRNGs — anywhere defined wraparound *is* the intent |
| **`promote`** | promotes the result to an arbitrary-precision integer (bigint) | yes | CRuby-faithful integer math (experimental, see below) |

The mode applies to integer `+`, `-`, `*`, unary `-`, and (under `promote`) `**`
and shifts. It does not change division: `1 / 0` is always a
`ZeroDivisionError` regardless of mode.

### `raise` (default)

The default refuses to be silently wrong. A computation that exceeds 64 bits is
almost always a bug or a case that needs `promote`; raising surfaces it at the
point it happens rather than producing a truncated value. This is a deliberate
deviation from CRuby (which would never raise here) in favour of loudness.

### `wrap`

`wrap` skips the overflow check entirely, so arithmetic is plain C wraparound.
Choose it when wraparound is the algorithm — hashing, checksums, fixed-width bit
manipulation, RNGs — not as a blanket "make overflow go away", since it will
silently truncate a value the program genuinely needed. It is the fastest mode
(no checks); for example the optcarrot build uses `wrap`.

### `promote`

`promote` makes integers behave like CRuby's: a result that exceeds 64 bits
becomes a bigint instead of overflowing. Small values stay unboxed machine
integers (like CRuby's fixnum), and only the ones that actually overflow pay the
bigint cost, so it is more practical than widening everything.

`promote` is **experimental**: most integer code works, but coverage is not yet
complete (some overflow paths through method arguments, closures, and certain
containers still raise rather than promote, and very large integer *literals*
are not yet represented). Treat it as opt-in CRuby fidelity, not a finished
guarantee. It also carries a runtime cost (bigint allocation and GC pressure),
so the default stays `raise`.

## Using it when you compile the C yourself

In the normal `spinel app.rb` flow the driver compiles and links in one step and
passes the matching `-DSP_INT_OVERFLOW_MODE_{RAISE,WRAP,PROMOTE}` to the C
compiler for you, so `--int-overflow=MODE` is all you need.

If you emit C with `-c` and compile it separately, the generated code and the
runtime must agree on the mode, so pass the same define to your own `cc`:

```sh
spinel app.rb --int-overflow=wrap -c -o app.c
cc app.c -DSP_INT_OVERFLOW_MODE_WRAP -Ilib libspinel_rt.a -lm -o app
```

## See also

- [limitations.md](limitations.md) — where Spinel's static, fixed-width model
  differs from CRuby, including integer precision.
