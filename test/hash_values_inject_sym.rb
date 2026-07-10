# inject/reduce(:sym) folds a poly (boxed-element) array -- Hash#values,
# Hash#map results -- via the tag-dispatching operators.
h = { a: 1, b: 2, c: 3 }
p h.values.inject(:+)
p h.values.reduce(:+)
p h.map { |k, v| v }.inject(:+)
p h.values.inject { |a, b| a + b }
p h.keys.length
