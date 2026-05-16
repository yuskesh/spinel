# #546. Sibling of #510 -- merge / dup / delete dispatch was
# missing on sym_int_hash (Symbol-key, Int-value). #510 fixed
# the same surface for sym_str_hash and sym_poly_hash. Spinel
# emitted `cannot resolve call to 'merge' on sym_int_hash
# (emitting 0)` and the call no-op'd.
#
# Trigger in roundhouse: `ActionView::ViewHelpers.link_to`
# builds an HTML-attrs hash via `.merge`; when the value type
# was numeric (sym_int_hash variant) the merge dropped silently.
#
# Fix:
# 1. spinel_codegen.rb: new `sp_SymIntHash_merge` runtime helper
#    (mirrors the sym_str_hash version with NULL-guard on the
#    second arg per the #542 / #546 NULL-receiver pattern).
# 2. spinel_codegen.rb: sym_int_hash recv dispatch block gains
#    merge / dup / delete arms.
# 3. spinel_codegen.rb: kwarg-shorthand merge (`h.merge(c: 3)`)
#    builds the matching sp_SymIntHash inline from the
#    KeywordHashNode pairs, mirroring the kwarg-as-bundle path
#    in compile_constructor_args.

def merge_kwarg(h)
  h.merge(c: 3, d: 4)
end

def merge_hash(h)
  h.merge({c: 3, d: 4})
end

# Both forms work post-fix.
r1 = merge_kwarg({a: 1, b: 2})
puts r1[:a]   # 1
puts r1[:b]   # 2
puts r1[:c]   # 3
puts r1[:d]   # 4

r2 = merge_hash({a: 10, b: 20})
puts r2[:a]   # 10
puts r2[:c]   # 3

# dup
h = {x: 100, y: 200}
h2 = h.dup
puts h2[:x]   # 100
puts h2[:y]   # 200

# delete
h3 = {p: 1, q: 2, r: 3}
h3.delete(:q)
puts h3[:p]   # 1
puts h3[:q]   # 0 (missing now)
puts h3[:r]   # 3
