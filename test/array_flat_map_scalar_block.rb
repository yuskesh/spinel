# flat_map whose block value is statically scalar is exactly map (CRuby
# appends non-array values as-is); array/poly block values keep the
# splicing emitters.
p [1, 2, 3].flat_map { |x| x }
p [1, 2, 3].flat_map { |x| [x, x] }
p [1, 2, 3].flat_map { |x| x * 10 }
p %w[ab cd].flat_map { |s| s.upcase }
p [1, 2, 3].collect_concat { |x| x + 1 }
p [[1, 2], [3]].flat_map { |a| a }
h = {a: 1}
p h.flat_map { |k, v| v }
