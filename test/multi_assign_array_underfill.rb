# Multiple assignment from a single array RHS with fewer elements than targets:
# the surplus targets land nil (`a, b, c = [10, 20]` -> c is nil). Previously
# rejected at compile time ("unsupported multiple assignment"); the surplus
# targets now nil-fill and the affected local widens to poly so it can hold nil.
# Receivers routed through a method param exercise the typed runtime path.
def s(x); x; end

# top-level under-fill: trailing target nil
a, b, c = s([10, 20])
p [a, b, c]

# two missing trailing targets
d, e, f = s([1])
p [d, e, f]

# string elements, nil tail
g, h, i = s(["x", "y"])
p [g, h, i]

# float elements, nil tail
j, k, l = s([1.5])
p [j, k, l]

# under-fill inside a method, result returned as an array literal (exercises
# method-return inference: the array must type as poly because of the nil tail)
def from_pair
  x, y, z = [1, 2]
  [x, y, z]
end
p from_pair

# the surplus nil target is usable; the supplied ones keep their real type
m, n = s([5])
p m + 1
p n.nil?

# exact length and over-supplied forms still behave
p1, p2, p3 = s([1, 2, 3])
p [p1, p2, p3]
q1, q2 = s([1, 2, 3])
p [q1, q2]
