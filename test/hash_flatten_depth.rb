# Hash#flatten(depth): to_a.flatten(depth) semantics -- depth >= 2 expands
# array values, 0 keeps the pairs, negative flattens completely (the
# argless interleave keeps its fast arm).
h = { a: [1, 2], b: 3 }
p h.flatten
p h.flatten(0)
p h.flatten(1)
p h.flatten(2)
p h.flatten(-1)
g = { x: [1, [2, 3]], y: 4 }
p g.flatten(2)
p g.flatten(3)
