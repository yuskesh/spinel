# Float#ceil / #floor / #round / #truncate return type

In CRuby these four methods choose their return class from the **runtime
value** of the `ndigits` argument:

| call            | CRuby returns      |
|-----------------|--------------------|
| `1.9.round`     | `2`    (Integer)   |
| `1.9.round(0)`  | `2`    (Integer)   |
| `1.9.round(-1)` | `0`    (Integer)   |
| `1.234.round(2)`| `1.23` (Float)     |

i.e. Integer when `ndigits <= 0`, Float when `ndigits > 0`.

## The rule spinel uses

The return type matches CRuby's value-based rule **whenever `ndigits`
is a literal integer** -- the common case:

- **no argument** -> `Integer`  (`x.round`, `x.ceil`, ...)
- **literal `ndigits <= 0`** -> `Integer`  (`x.round(0)`, `x.round(-1)`)
- **literal `ndigits > 0`** -> `Float`  (`x.round(2)`)

For an `ndigits <= 0` result the float formula is cast to `mrb_int`, so
both the value and `#class` match CRuby (`1234.5.round(-1)` is the
Integer `1230`).

## The residual divergence

A **non-literal** `ndigits` keeps the static `Float` type:

- `x.round(n)` with a variable `n` would need an Integer-or-Float
  return type chosen at runtime. Forcing it into a boxed/poly value to
  carry both would discard static typing for every expression the
  result flows into, which runs against spinel's static-typing design.
- `x.round(*args)` / `x.round(*[])` can't be classified statically:
  whether any argument is present, and its sign, are runtime facts.

So `x.round(n)` where `n` is a variable holding a non-positive value
returns a Float (`10.0`) where CRuby returns an Integer (`10`). The
**value is still computed exactly** (`1.234.round(n)` with `n == 2` is
`1.23`, `1234.5.round(n)` with `n == -1` is `1230.0`); the values are
numerically equal to CRuby's and only `#class` and the default string
form (`"10.0"` vs `"10"`) differ. Code that needs an Integer can convert
explicitly: `x.round(n).to_i`.

The implementation lives in `infer_call` (`src/analyze_infer.c`) and
`emit_scalar_call` (`src/codegen_call.c`); both classify a literal-integer
`ndigits` by sign and fall back to an exact Float computation otherwise.
