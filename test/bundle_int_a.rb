# Bundled tests:
#   - int_array_literal_lshift_heterogeneous
#   - int_array_replace_expr
#   - int_eq_nil_strict

# === int_array_literal_lshift_heterogeneous ===
def t_int_array_literal_lshift_heterogeneous
  # `<int_array literal> << <non-int>` shape (`[1, 2] << "c"`).
  # Pushing a non-int element onto a literal int_array previously
  # emitted sp_IntArray_push with a pointer-from-int-conversion
  # warning and produced a broken array. The contextual lift
  # rebuilds the recv as a poly_array of box_int(...) elements and
  # pushes the boxed arg, matching CRuby's heterogeneous-array
  # semantic.
  #
  # Scoped to direct ArrayNode literals on the LHS, so a stored
  # ivar / local (`@slots = [nil] * N; @slots << ...`) keeps the
  # existing observation-widening path. Issue #619 puzzle 7.
  p ([1, 2] << "c") == [1, 2, "c"]       # true
  p ([1, 2] << nil) == [1, 2, nil]       # true
  p ([1, 2] << 3) == [1, 2, 3]           # true (int <<, no lift)
end
t_int_array_literal_lshift_heterogeneous

# === int_array_replace_expr ===
def t_int_array_replace_expr
  # `Array#replace` in *expression* position on an int_array.
  # The stmt-form arm has long supported this; the expr-form was
  # missing — unresolved-call warning + literal `0` emitted.
  # Surfaced via optcarrot's `def load_battery; ...; @wrk.replace(sav.bytes); end`,
  # where the call sits at the tail of the method (its value is
  # the implicit return).
  
  a = [1, 2, 3]
  b = [10, 20, 30, 40]
  
  # Expression position: assigned. Should yield the (mutated) `a`.
  c = a.replace(b)
  
  # `a` mutated in place
  puts a[0]
  puts a[1]
  puts a[2]
  puts a[3]
  puts a.length
  
  # `c` is the same array (replace returns the receiver)
  puts c[0]
  puts c.length
end
t_int_array_replace_expr

# === int_eq_nil_strict ===
def t_int_eq_nil_strict
  # Partial fix for #521. In CRuby `0 == nil` is false -- only nil
  # equals nil. Spinel used to emit `(lc op rc)` for `int == nil`,
  # where `rc` was "0" (compile_expr of NilNode); since unboxed ints
  # share the C representation with the nil sentinel, this conflated
  # stored 0 with nil and made `0 == nil` return true.
  #
  # Fix: compile_eq has explicit value-type-vs-nil arms that
  # constant-fold to FALSE (or TRUE for `!=`). This does NOT solve
  # the deeper #521 problem -- Hash<String,Int> still returns 0 for
  # missing keys -- but it does fix the categorical bug that made a
  # *stored* 0 in the hash indistinguishable from "the value is
  # nil." With this fix, `v != nil` reflects the static type
  # honestly, so users who need to distinguish missing-vs-stored
  # zero have to reach for `Hash#has_key?` / `Hash#fetch`, as
  # documented in the issue comment.
  
  # Pure int (no hash)
  v = 0
  puts (v == nil).inspect   # false
  puts (v != nil).inspect   # true
  
  w = 5
  puts (w == nil).inspect   # false
  puts (w != nil).inspect   # true
  
  # Float / bool same.
  f = 0.0
  puts (f == nil).inspect   # false
  puts (f != nil).inspect   # true
  
  b = false
  puts (b == nil).inspect   # false
  
  # Hash<String,Int> stored 0 case now reports != nil correctly.
  h = {}
  h["x"] = 0
  h["y"] = 5
  vx = h["x"]
  vy = h["y"]
  puts (vx == nil).inspect  # false (was true)
  puts (vx != nil).inspect  # true  (was false)
  puts (vy == nil).inspect  # false
  puts (vy != nil).inspect  # true
  
  # Symmetric: nil == int is also false.
  puts (nil == 0).inspect   # false
  puts (nil != 0).inspect   # true
end
t_int_eq_nil_strict

