# Splat on the RHS of a multiple assignment (`*a = *x`), rest-only targets
# under a scalar RHS (`*a = 5` -> [5]), and nil splats contributing nothing.
*a = *[1, 2]
p a
*b = *nil
p b
*c = 5
p c
d, *e = *[1, 2, 3]
p d
p e
f, *g = 7
p f
p g
h = [*nil]
p h
def r(val)
  x = yield
  val == x
end
p r([1]) { next *1 }
