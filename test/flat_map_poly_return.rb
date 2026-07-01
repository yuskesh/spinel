# flat_map whose block value is not statically an array -- a bare poly, or a mix
# of array and scalar as in `cond ? sub_array : scalar` -- follows CRuby: an array
# value is spliced one level, a scalar is appended as-is. Distinct per-case
# helpers keep each receiver's element type clean; the method param defeats
# constant folding of the receiver.
def pa(x) = x
def pb(x) = x
def pc(x) = x
def pe(x) = x

p pa([1, [2, 3], 4]).flat_map { |e| e.is_a?(Array) ? e : e }        # [1, 2, 3, 4]
p pb([1, [2, 3], 4]).flat_map { |e| e.is_a?(Array) ? e : [e, e] }   # [1, 1, 2, 3, 4, 4]
p pc([:x, [:y, :z], :w]).flat_map { |e| e.is_a?(Array) ? e : e }    # [:x, :y, :z, :w]

# a `next` that carries a mixed value flattens like the tail
p pe([1, [2], 3]).flat_map { |e| next e if e.is_a?(Array); e }      # [1, 2, 3]
