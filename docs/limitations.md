# Spinel limitations: what an AOT compiler can and cannot do

Spinel is a whole-program **ahead-of-time** compiler: it reads the entire
program, infers a static type for every value, emits C, compiles it, and runs
the binary. There is no Ruby interpreter, parser, or type-inference engine in
the running program — it is just C. That model is what buys the speedup, and it
is also the source of every limitation below.

This document is the honest catalogue. It is organized by *kind* of limit:

- **Fundamental** — incompatible with whole-program AOT; will not change without
  abandoning the model (e.g. bundling an interpreter).
- **Partial / relaxable** — genuinely limited today, but additively fixable.
- **By design** — a deliberate, documented choice; the intentional CRuby
  deviations are catalogued under [By design](#by-design-deliberate-choices)
  below.
- **Now supported** — things that are *not* limits (corrects older write-ups
  that described an earlier version of the compiler).

---

## Fundamental limits (inherent to AOT)

These need a runtime parser, a runtime metaobject protocol, an allocation
registry, or stack reification — none of which exist in a flat compiled binary.

| Feature | Behaviour | Why it's fundamental |
|---|---|---|
| `eval` / `instance_eval("str")` / `class_eval("str")` | unsupported | needs a runtime parser + type system. (Block forms — `instance_eval { }` — DO work; the block is compiled.) |
| `method_missing` | not dispatched (defining it warns at compile time) | every call site is a direct C call; an undefined-method call can't fall back to a per-receiver hook. The method is still callable explicitly. |
| `define_method` with a runtime-computed name/body | only literal names work | a runtime-built method has no compiled body |
| `ObjectSpace` (`each_object`, `count_objects`) | unsupported | no class-keyed allocation registry; the GC tracks bytes, not a live-object index |
| `TracePoint` / `set_trace_func` / `binding` | unsupported | require an interpreter loop and reified local scopes |
| Refinements (`refine` / `using`) | no-op / unresolved | scope-keyed dispatch is incompatible with direct C calls |
| `callcc` / `Continuation` | unsupported | multi-shot full-stack capture has no flat-C analogue |
| `Class.new(parent) { ... }` (runtime class) | unsupported | the class graph is baked at compile time |
| General reflection (`instance_variable_get(var)`, `methods`, `instance_variables`) | unsupported | ivars are C struct offsets with no name→offset table; DCE strips method names |
| User-defined `#hash` / `#eql?` for hash *keys* | not dispatched (identity probe) | the hash machinery can't call back into a user method per key |
| `require` of stdlib `.rb` (e.g. `time`, `set`, `json/pure`) | unsupported | stdlib leans on metaprogramming / C extensions off the AOT path |
| `Marshal` of arbitrary objects | unsupported | load needs a runtime class registry keyed by name |
| Mixed / non-UTF-8 encodings | UTF-8 / ASCII-8BIT only | one internal representation; transcoding tables are out of scope |
| Embedded `NUL` in general binary strings | `char *` boundary assumption | most string ops are NUL-terminated at the C boundary |

`send`/`public_send`/`__send__` with a **non-literal** name (`send(meth)`) is in
this group: the target can't be resolved statically. A **literal** name is
supported — see below.

---

## Partial / relaxable limits

Limited today, but additively fixable; listed roughly easiest-first.

| Feature | Today | Path to relax |
|---|---|---|
| `Exception#backtrace` / `Kernel#caller` | return `[]` (class + message work) | populate frames from a compile-time call-site→source side-table (the `--line-map` map already exists) |
| `Thread` real parallelism | `Thread`/`Mutex` are modelled as single-threaded Fibers (`Thread.new{}.value` works, `synchronize` runs inline) | true parallelism needs a concurrent GC and scheduler — large |
| `Marshal` of primitives (Int/Float/String/Array/Hash) | unsupported | a fixed wire format — medium; arbitrary objects stay fundamental |
| Mixin/inheritance lifecycle hooks (`included` / `inherited` / `extended`) | defined but not fired | emit a startup call with the literal class arg (the include/inherit graph is known at compile time) |
| External `Enumerator` (`.each`/`.map` with no block → `.next`/`.peek`/lazy) | unsupported | needs Fiber-style suspension — large. Chained block→`.to_a` forms (`each_slice(n).to_a`, `filter_map`, `map{}.to_a`) already work. |
| `Array#hash` (and arrays as hash keys) | unsupported | a builtin is additive, but array *keys* need the fundamental key-dispatch above |

---

## By design (deliberate choices)

- **Integer overflow** — pick one mode at compile time: `raise` (default,
  `RangeError` on overflow) or `--int-overflow=promote` (auto-bignum). Not both
  in one binary, because the representation is chosen statically.
- **Float `round(ndigits)`** — the value is always correct; the *return class*
  follows CRuby (Integer for `round` with 0 digits, Float otherwise).
- **Frozen literals** — explicit `.freeze` then mutation raises `FrozenError`,
  matching CRuby. (String literals are *not* implicitly frozen — see below.)

### Intentional incompatibilities with CRuby

Spinel aims to be a subset of Ruby: programs it accepts should behave the same
as on CRuby. In a few cases CRuby's behavior depends on a feature Spinel does
not implement, and silently returning a wrong value would be worse than a
visible error. Those deliberate divergences are listed here.

#### `Integer#**` with a negative exponent

CRuby evaluates a negative integer exponent to a `Rational` (`2 ** -1 # => (1/2)`).
Spinel now has a `Rational` type, but `Integer#**` keeps a static `Integer`
result type: the exponent's sign is generally not known at compile time, so
typing `x ** y` as a sometimes-`Rational` would force the result poly and
cascade through inference. A negative integer exponent therefore raises rather
than silently truncating to `0`:

```ruby
2 ** -1   # RangeError: negative exponent
```

This applies to `Integer#**` / `Integer#pow` across the int, bigint, and
poly-dispatched paths, and is catchable as usual (`rescue RangeError`).
`Float#**` is unaffected and stays CRuby-compatible, because a float result is
representable (`2.0 ** -1 # => 0.5`). A `Rational` base is also fine
(`Rational(1,2) ** -1 # => (2/1)`).

#### `Rational` precision and `Complex` components

`Rational` is stored as a pair of fixed `mrb_int` numerator/denominator. The
arithmetic is exact while the reduced terms fit in `mrb_int`; an operation whose
result would overflow raises `RangeError` rather than promoting to a Bigint as
CRuby does:

```ruby
Rational(10**18, 1) * Rational(10**18, 1)   # RangeError (CRuby: a Bigint Rational)
```

`Complex` is stored as a pair of `mrb_float` components, so unlike CRuby it does
not preserve `Integer`/`Rational` component types. Operators and display match
CRuby for the common cases, but `#real` / `#imaginary` / `#abs2` return `Float`,
and exact division yields `Float` components instead of `Rational`:

```ruby
Complex(1, 2).real          # => 1.0   (CRuby: 1)
Complex(1, 2) / Complex(3, -1)   # => (0.1+0.7i)   (CRuby: ((1/10)+(7/10)*i))
```

Storing a `Rational` or `Complex` in a heterogeneous (poly) array is not yet
supported -- the 16-byte value does not fit the boxed-value union, so such an
element reads back as `nil`.

#### `String#grapheme_clusters`

Correct Unicode extended-grapheme segmentation (`"á".grapheme_clusters # => ["á"]`)
requires shipping and maintaining the Unicode grapheme-break property tables,
which Spinel deliberately does not carry. `String#grapheme_clusters` and
`String#each_grapheme_cluster` are therefore not supported. For codepoint- or
byte-level iteration, use the supported `String#chars`, `#each_char`,
`#codepoints`, or `#bytes`.

#### Aliasing the regexp match globals

CRuby's `English` library aliases the punctuation match globals to readable
names (`alias $MATCH $&`, etc.). In Spinel the match globals (`$&`, `` $` ``,
`$'`, `$+`, `$~`) are not ordinary global-variable storage: a direct read lowers
to a special regexp runtime accessor. Supporting `alias $name $&` would require a
separate special-global alias mechanism plus broader `MatchData` compatibility,
outside the intended AOT subset. Aliasing one of these globals is rejected at
compile time rather than falling through to an undefined generated symbol:

```
$ spinel uses_english.rb
Error: global aliasing of regexp special globals is not supported (alias $MATCH $&)
```

Direct reads of the match globals work as usual; only aliasing them is
unsupported, so `require "English"` does not compile.

#### Flip-flop operator

CRuby supports the flip-flop operator (a `Range` used as a condition, toggled
between its two endpoints): `puts i if (i == 3)..(i == 5)`. This is a rarely used
feature with surprising hidden per-site state, and Spinel does not support it; a
program using it fails to compile rather than running with wrong behavior. Use an
explicit boolean state variable instead.

---

## Now supported (older write-ups are stale here)

These were limits in an earlier (Ruby self-hosted) version of the compiler and
now work on current master:

| Feature | Status |
|---|---|
| Mutable string literals (`s = "x"; s << "y"`; `"x".frozen?` → `false`) | works |
| Hash missing key → `nil` (string- and int-keyed, including `Hash.new(default)`) | works |
| `define_method(:name) { ... }` with a literal name | works |
| Block-param arity (un-yielded params are `nil`, not a sentinel) | works |
| Closures flowing through containers (`{op: ->(a,b){a+b}}[:op].call(2,3)`) | works |
| `String#oct` (`0x`/`0b`/`0o` prefixes) and `Array#first` on empty → `nil` | works |
| `send(:literal)` / `__send__("literal")` / `public_send(:literal)` on **implicit self** | works (resolved on the AST, so a `send(:` inside a string literal is left untouched) |
| Hash variant inference (a wrong initial guess widens to poly transparently) | correct (a perf cost, not a correctness limit) |

There is no Ruby self-host "bootstrap fixpoint" constraint: the C compiler is
the master implementation.

---

## Why this still works

Most real programs use the dynamic features above sparingly, in setup code, or
not at all. Spinel targets the large static core of Ruby — classes, methods,
blocks, the collection protocols, exceptions, mixins — and compiles it to fast
native code. When a program does need a feature in the *fundamental* table, that
program is not a fit for AOT; for everything else, the limits are either by
design or on the relaxable list.
