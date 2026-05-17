# #559 (Sam Ruby). Sibling of #551. sym_poly_hash.merge with
# a positional sym_int_hash argument was missing dispatch
# (only the KeywordHashNode and same-variant shapes were
# wired). The cross-variant lowering routes through a new
# sp_SymPolyHash_merge_int runtime helper that boxes each
# int value as sp_box_int into the result poly slot.

def f
  h = { a: "x", b: 1 }
  other = { c: 2 }
  h.merge(other)
end

m = f
puts m.length
puts m.has_key?(:a)
puts m.has_key?(:b)
puts m.has_key?(:c)
