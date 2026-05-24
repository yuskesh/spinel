# Bundled tests:
#   - poly_array_clear_expr
#   - poly_array_eq_all_nil_literal

# === poly_array_clear_expr ===
def t_poly_array_clear_expr
  # `Array#clear` in *expression* position on a poly_array. The
  # stmt-form was already supported; the expr-form fell through
  # to the unresolved-call warning and emitted literal 0, which
  # made `c = a.clear` mistype `c` as int.
  #
  # Surfaced via optcarrot's `@sp_visible ||= @sp_map.clear` shape
  # (the `||=` form needs a typed initializer here so Spinel's
  # per-variable type inference has a concrete poly_array tag to
  # unify with — Ruby allows type changes across reassignment but
  # Spinel doesn't).
  
  a = [1, "two", :three, [4, 5]]
  b = a.clear
  
  puts a.length    # 0
  puts b.length    # 0 (b is a, just emptied — same array, just mutated)
  
  # Subsequent push refills from index 0.
  a.push(99)
  puts a.length    # 1
end
t_poly_array_clear_expr

# === poly_array_eq_all_nil_literal ===
def t_poly_array_eq_all_nil_literal
  # `<poly_array> == [nil, nil, ...]` shape. The RHS array literal
  # would otherwise lower to int_array of zeros and fall through to
  # a raw pointer compare. The contextual lift at the == call site
  # rebuilds the RHS as a poly_array of box_nil() and dispatches
  # through sp_PolyArray_eq, matching the CRuby semantic that
  # `Array.new(N) == [nil] * N` is true. Issue #619 puzzle 4.
  p Array.new(3) == [nil, nil, nil]   # true
  p Array.new(0) == []                # true (empty arrays: handled by the IntArray fallback)
  p Array.new(2) == [nil, nil]        # true
  p Array.new(3) != [nil, nil, nil]   # false
end
t_poly_array_eq_all_nil_literal

