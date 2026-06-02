# Intentional incompatibilities with CRuby

Spinel aims to be a subset of Ruby: programs it accepts should behave the same
as on CRuby. In a few cases CRuby's behavior depends on a feature Spinel does
not implement, and silently returning a wrong value would be worse than a
visible error. Those deliberate divergences are listed here.

## `Integer#**` with a negative exponent

CRuby evaluates a negative integer exponent to a `Rational`:

```ruby
2 ** -1   # => (1/2)
2 ** -2   # => (1/4)
```

Spinel has no `Rational` type. Rather than silently truncating the result to
`0` (the previous behavior), a negative integer exponent raises:

```ruby
2 ** -1   # RangeError: negative exponent
```

This applies to `Integer#**` / `Integer#pow` across the int, bigint, and
poly-dispatched paths. It is catchable as usual:

```ruby
begin
  2 ** -1
rescue RangeError => e
  # e.message == "negative exponent"
end
```

`Float#**` is unaffected and stays CRuby-compatible, because a float result is
representable:

```ruby
2.0 ** -1   # => 0.5
```
