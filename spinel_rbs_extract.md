# spinel_rbs_extract — supported RBS subset

`spinel_rbs_extract` reads `*.rbs` files and emits a line-oriented seed
file that `spinel_analyze` consumes when invoked with `spinel --rbs DIR`.
The seeded types are **advisory**: inference still runs on top, and the
analyzer widens on observed contradiction, so the seed file is never
load-bearing — a wrong or unrepresentable seed is at worst a no-op.

Anything outside the subset below is silently dropped: the seed line
isn't emitted, and the analyzer falls back to its normal inference for
that method or ivar.

## Supported

### Type vocabulary

| RBS                                    | Spinel tag                            |
| -------------------------------------- | ------------------------------------- |
| `Integer`                              | `int`                                 |
| `Float`                                | `float`                               |
| `String`                               | `string`                              |
| `Symbol`                               | `symbol`                              |
| `TrueClass`, `FalseClass`              | `bool`                                |
| `NilClass`, `nil`                      | `nil`                                 |
| `bool`                                 | `bool`                                |
| `void` (return only)                   | `nil`                                 |
| `Foo`, `Foo::Bar` (nominal)            | `obj_Foo`, `obj_Foo_Bar`              |
| `Array[Integer]`                       | `int_array`                           |
| `Array[Float]`                         | `float_array`                         |
| `Array[String]`                        | `str_array`                           |
| `Array[Symbol]`                        | `sym_array`                           |
| `Array[Foo]`                           | `obj_Foo_ptr_array`                   |
| `Array[<other>]`                       | `poly_array`                          |
| `Hash[String, Integer]`                | `str_int_hash`                        |
| `Hash[String, String]`                 | `str_str_hash`                        |
| `Hash[String, <other>]`                | `str_poly_hash`                       |
| `Hash[Symbol, Integer]`                | `sym_int_hash`                        |
| `Hash[Symbol, String]`                 | `sym_str_hash`                        |
| `Hash[Symbol, <other>]`                | `sym_poly_hash`                       |
| `T?`                                   | `<T>?`  (recursive)                   |
| `T \| nil` / `nil \| T`                | `<T>?`                                |

### Members emitted

- `def name: (...) -> R` — instance method (`meth`)
- `def self.name: (...) -> R` — class method (`cmeth`)
- `def self?.name: (...) -> R` — emits both `meth` and `cmeth`
- `attr_accessor`, `attr_reader`, `attr_writer` — emits `ivar`

### Unqualified type resolution

Inside `module Foo; class Bar`, an unqualified reference like
`def record: () -> Base` is resolved to `obj_Foo_Base` (single-level
parent fallback). A wrong guess is a no-op because the analyzer
silently drops seeds for unknown types — no symbol table is required.
Covers the common sibling-in-module pattern; full lexical lookup is
not implemented.

## Dropped (silently skipped)

### Method signature shapes

- Multiple overloads — only the first overload is considered; if the
  first is itself out-of-subset the whole method is skipped.
- Optional positional params (`?String`)
- Rest positional params (`*args`)
- Trailing positional params
- Required keyword params (`name: T`)
- Optional keyword params (`?name: T`)
- Rest keyword params (`**rest`)
- Block params (`{ ... }`, `?{ ... }`)
- Generic method params (`[X]`)
- Proc / untyped function types

A signature that touches any of the above is dropped wholesale rather
than emitted partially.

### Param-level — any one of these in any required positional kills the whole signature

- A type that doesn't reduce to a supported tag.
- A generic container that isn't `Array[T]` or `Hash[K, V]`.
- A union that isn't `T | nil`.

### Type-level

- `self`, `instance`, `class`, `top`, `bot`, `untyped` / `any`
- Interfaces (`_Foo`)
- Intersections (`T & U`)
- Literal types (`:foo`, `42`, `"x"`)
- Type aliases
- Type variables (generics with parameters)
- Records (`{ name: T, ... }`)
- Tuples (`[T, U]`)
- Proc types (`^(T) -> U`)
- `Hash[K, V]` where K is not `String` or `Symbol`

### Members

- `include`, `extend`, `prepend` — mixin ancestry not modeled
- `public`, `private`
- `alias`
- `@ivar` / `@@cvar` declarations (use `attr_*` for ivars)

## Seed file format

Consumed by `load_rbs_seeds` / `apply_rbs_seeds` in
`spinel_analyze.rb`:

```
class <QualifiedName>           # enter class scope; nested names use `_`
meth <name> <ret> <ptypes>      # `-` means "leave alone"; ptypes is
cmeth <name> <ret> <ptypes>     # comma-separated, or `-` for nullary
ivar <name> <type>
```

Lines whose first token isn't a keyword are treated as comments.

See `experiments/rbs/box.seed` for a worked example.

## Follow-up

Tracked as out-of-scope for the initial spike (see matz/spinel#7 +
OriPekelman/tep#6):

- Multi-overload resolution
- Generics with type variables
- Mixin ancestry from `include` / `extend`
- Return-type pinning where the body would otherwise cascade-widen
