# Bundled tests:
#   - hash_new_default_value
#   - hash_or_fallback_missing_key

# === hash_new_default_value ===
def t_hash_new_default_value
  # `Hash.new(default_value)` per-instance default support.
  #
  # Each hash variant struct (StrIntHash / StrStrHash / IntStrHash /
  # SymIntHash / SymStrHash / StrPolyHash / SymPolyHash / PolyPolyHash)
  # gains a `default_v` field. `_new` initializes it to the variant's
  # sentinel zero (so legacy `{}` literals unchanged); `_new_with_default`
  # is a new constructor that sets the field. `_get` returns
  # `default_v` on miss; `_dup` / `_merge` propagate (merge inherits
  # the LEFT receiver's default per CRuby).
  #
  # Codegen routes `Hash.new(N)` through `_new_with_default` and
  # adds `Hash#default` / `Hash#default=` accessor arms to each
  # variant. The analyzer's `infer_call_type` arm picks the right
  # variant from the default's type (string → str_str_hash, int
  # / nil / bool → str_int_hash) and types `h.default` as the
  # value-slot type so `puts h.default` dispatches correctly.
  #
  # Block form `Hash.new { |h, k| ... }` (proc default) remains
  # deferred -- separate feature.
  #
  # Issue #600 puzzle 2.
  
  # Int default
  h = Hash.new(5)
  puts h[:a]              # 5
  puts h["x"]             # 5
  puts h.length           # 0
  puts h.key?(:a)         # false
  puts h.default          # 5
  
  # Modify default; existing key unaffected
  h.default = 99
  puts h["missing"]       # 99
  h["b"] = 10
  puts h["b"]             # 10
  puts h["c"]             # 99
  
  # String default
  s = Hash.new("none")
  puts s["x"]             # none
  puts s.default          # none
  
  # dup propagates default
  h2 = Hash.new(42)
  h2["a"] = 1
  d = h2.dup
  puts d.default          # 42
  puts d["never_set"]     # 42
  puts d.length           # 1
  puts d["a"]             # 1
  
  # merge inherits left default
  m = h2.merge({"x" => 100})
  puts m.default          # 42
  puts m["not_there"]     # 42
  
  # Hash.new(0) -- pre-existing lrama_features shape, default ==
  # sentinel, behaviour unchanged.
  counter = Hash.new(0)
  counter["x"] = counter["x"] + 1
  puts counter["x"]       # 1
  puts counter["z"]       # 0

  # Float default with int keys -- lands on the PolyPoly variant, which
  # previously had no default_v / _new_with_default at all, so
  # `Hash.new(0.0)` emitted `sp_PolyPolyHash_new_with_default(0.0)` and
  # failed C compilation (int-to-pointer conversion).
  f = Hash.new(0.0)
  f[1] += 2.5
  p f[1]                  # 2.5
  p f[7]                  # 0.0 (default for a missing key)
  p f.default             # 0.0
  f.default = 1.5
  p f[9]                  # 1.5
  fd = f.dup
  p fd[42]                # 1.5 (dup propagates default)
  fm = f.merge({2 => 3.5})
  p fm[2]                 # 3.5
  p fm[99]                # 1.5 (merge inherits left default)
end
t_hash_new_default_value

# === hash_or_fallback_missing_key ===
def t_hash_or_fallback_missing_key
  # Issue #660: `hash[k] || rhs` on a typed-hash receiver must return
  # rhs when the key is missing -- not the value-type zero (0 / "").
  #
  # Background: typed hash variants (sp_StrIntHash, sp_StrStrHash, ...)
  # materialize the value type's zero as the missing-key default. Combined
  # with the `||` truthy-lhs ternary lowering, the rhs arm was unreachable:
  # 0 / "" are truthy in Ruby, so the always-present zero short-circuited
  # the fallback.
  #
  # Fix: compile_or detects `hash[k]` on a typed-hash recv and rewrites
  # to `has_key(h, k) ? get(h, k) : rhs`. Poly-hash variants already
  # return sp_box_nil on miss, so they keep the legacy truthy lowering.
  
  # Empty hash, no writes (inferred str_int_hash).
  h1 = {}
  puts (h1[:missing] || "fallback")
  
  # String-valued hash via prior write.
  h2 = {}
  h2[:set_key] = "value"
  puts (h2[:missing] || "fallback")
  
  # Int-valued hash via prior write -- the missing-key 0 used to win.
  h3 = {}
  h3[:set] = 1
  puts (h3[:missing] || -1)
  
  # Present key returns the value, not the fallback.
  h4 = {}
  h4[:found] = "got it"
  puts (h4[:found] || "fallback")
end
t_hash_or_fallback_missing_key

