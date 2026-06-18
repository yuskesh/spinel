# Issue #829: array.each_slice(n).map { |x, y| ... } chains.
a = [1, 3, 6, 10]
p a.each_slice(2).map { |x, y| y - x }

# A second chain to verify reuse.
b = [2, 4, 8, 16]
p b.each_slice(2).map { |x, y| y * x }

# A single destructured param |(lo, hi)| splits the slice into its elements.
c = [1, 3, 6, 10]
p c.each_slice(2).map { |(lo, hi)| hi - lo }
