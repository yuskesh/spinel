# map!.with_index / collect!.with_index: collect like map.with_index,
# then write the result back into the receiver in place; the chain
# evaluates to the mutated receiver.
a = [1, 2, 3]
a.map!.with_index { |x, i| x + i }
p a
s = %w[a b]
s.map!.with_index { |x, i| x * (i + 1) }
p s
r = [5, 6].collect!.with_index(10) { |x, i| x + i }
p r
