# Array#fill with a value incompatible with the element type rebuilds the
# result as a poly array (CRuby just mixes the element in); positional and
# range spans, and the compatible typed fills keep their arms.
p [1, 2, 3, 4, 5].fill("x", 1, 2)
p [1, 2, 3, 4, 5].fill("x", 1..3)
p [1, 2, 3].fill(9)
a = [1, 2, 3, 4, 5]
a.fill("y", 2, 1)
p a
p %w[a b c].fill("z", 1)
