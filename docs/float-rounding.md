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

## Non-literal ndigits

A **non-literal** `ndigits` is classified at runtime: the result is a boxed
value that is an Integer when `ndigits <= 0` and a Float when `ndigits > 0`,
matching CRuby's class exactly.

- `x.round(n)` with a variable `n` types as a poly (boxed) value and emits a
  runtime branch on the sign of `n`, so both the value and `#class` match
  CRuby (`x.round(n)` with `n == -1` returns the Integer `1230`).

The one case still classified statically as `Float` is a splat whose presence
and sign are not knowable at all:

- `x.round(*args)` / `x.round(*[])` can't be classified statically -- whether
  any argument is present, and its sign, are runtime facts the arity model does
  not carry.

The implementation lives in `infer_call` (`src/analyze_infer.c`) and the Float
`round`/`floor`/`ceil`/`truncate` arm (`src/codegen_call_recv.c`): a
literal-integer `ndigits` is classified by sign, and a non-literal one emits a
runtime Integer-or-Float branch as a boxed value.
