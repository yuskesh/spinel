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
| General reflection (`methods`, `instance_variables`) and `instance_variable_get`/`set` with a **non-literal** name | unsupported | ivars are C struct offsets with no name→offset table; DCE strips method names. A **literal** `instance_variable_get(:@x)` / `instance_variable_set(:@x, v)` *is* supported — it resolves to the known struct offset, like `send(:literal)` below. |
| User-defined `#hash` / `#eql?` for hash *keys* | not dispatched (identity probe) | the hash machinery can't call back into a user method per key |
| `require` of stdlib `.rb` that leans on metaprogramming / C extensions (e.g. `json/pure`, the `require "time"` parsing extensions like `Time.parse` / `Time.strptime`) | unsupported | such stdlib code runs off the AOT path. A `require` is resolved at parse time by splicing a bundled `lib/X.rb`; the libraries that ship this way — `set`, `forwardable`, `optparse`, `erb`, `stringio`, `strscan` — do work. The built-in `Time` class (`Time.now` / `at` / `local` / `utc`, plus `strftime` / `iso8601` / `zone`) works *without* any `require`; only the `require "time"` string-parsing additions are missing. |
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
| `Marshal` of user objects with container-typed ivars | primitives + Array + Hash + Bignum + Complex + Rational + plain user objects work, including cyclic and shared references (`Marshal.dump`/`load`, CRuby 4.8 wire format, byte-compatible for the supported subset); an object whose ivar is a *statically typed* Array/Hash (not a poly ivar) is not yet dumpable | a user object dumps/loads through a compile-time-generated per-class dispatcher. Supported ivar types: scalars (Integer/Float/String/true/false/Symbol/Bignum), `poly` (mixed) ivars, and nested user objects. A typed-container ivar would mismatch the loader's always-poly containers, so such a class raises `TypeError` on dump; value-type and Exception-subclass objects are also out of scope. Complex's components are float-only, so they round-trip as Floats |
| Mixin/inheritance lifecycle hooks (`included` / `inherited` / `extended`) | defined but not fired | emit a startup call with the literal class arg (the include/inherit graph is known at compile time) |
| `Fiber` body reassigning a captured heap object | rebinding a captured String / Array / Hash to a *new* object (`s = "new"`, vs the supported in-place `s << x`) is not shared back to the enclosing scope | enclosing *value* locals (Integer/Float/Boolean/poly) are shared via a heap cell, and a captured heap object is shared by pointer (so in-place mutation reaches the outer scope), but rebinding the variable to a fresh object does not. A nested block (`Fiber.new { 3.times { \|i\| ... } }`) and a nested `Fiber.new` / `Enumerator.new` now compile and run |
| External `Enumerator` — `.each` with no block is only an Enumerator on `Array` / `Range`, not on an arbitrary user method | mostly supported | `Array#each` / `Range#each` with no block return a working external Enumerator (`#next` / `#peek` / `#rewind` / `#size`, `loop` stops on `StopIteration`). `Enumerator.new { \|y\| ... }` is a fiber-backed generator (`y << v`, `y.yield(v)`, and the bare `y.yield v` without parentheses, plus `#next` / `#peek` / `#rewind` / `#take` / `#first`, infinite generators work). `Enumerator::Lazy` over an int range (incl. endless) or int array fuses map/select/reject/filter/take_while chains terminated by `first(n)` / `to_a` / `force`. Chained block→`.to_a` forms (`each_slice(n).to_a`, `filter_map`, `map{}.to_a`) also work. |
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

#### `defined?`

`defined?` is resolved **statically at compile time** from the operand's
syntactic form and whole-program symbol presence, not from the runtime state of
the actual receiver or binding. It returns a fixed label string (or `nil`),
which matches CRuby for the common forms but differs in several cases. A full
runtime-accurate `defined?` would require carrying per-object/per-binding
definedness into the generated code; that cost is deliberately not paid.

Forms that match CRuby: a local variable (`"local-variable"`), a set/unset
instance variable, a set/unset global variable, a user or built-in constant
name (`"constant"`), a no-receiver call to a **user-defined** method
(`"method"`), `self`, `nil`/`true`/`false`, and the int/float/string/symbol/array
literals that report `"expression"`.

Where Spinel returns `nil` but CRuby returns a label:

| Operand | CRuby | Spinel |
| --- | --- | --- |
| `Foo::Bar` (constant path) | `"constant"` | `nil` |
| `puts` (built-in / Kernel method) | `"method"` | `nil` |
| `obj.meth` (call with a receiver) | `"method"` | `nil` |
| `1 + 1` (operator = method call) | `"method"` | `nil` |
| `{a: 1}`, `1..3` (hash/range and other general expressions) | `"expression"` | `nil` |
| `x = 1` (assignment) | `"assignment"` | `nil` |
| `yield`, `super` | `"yield"` / `"super"` | `nil` |

Two forms report a label where CRuby would report `nil`, because the check is
syntactic rather than runtime:

- An instance variable reports `"instance-variable"` when **any** code in the
  program assigns that ivar name -- not whether it is set on the specific
  receiver at that point.
- A class variable read always reports `"class variable"`, with no
  definedness check, so `defined?(@@undefined)` is `"class variable"` in Spinel
  versus `nil` in CRuby.

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
