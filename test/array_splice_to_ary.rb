# A splice RHS that defines to_ary is coerced through it (CRuby rb_ary_to_ary):
# the coercion's elements are spliced, while the expression value stays the
# object as written. An object without to_ary inserts as a single element.
class IntPair
  def initialize(a, b); @a = a; @b = b; end
  def to_ary; [@a, @b]; end
end

class Tagged
  def initialize(v); @v = v; end
  def to_ary; ["<", @v, ">"]; end
end

class Opaque
  def initialize(v); @v = v; end
end

# typed receiver, to_ary elements match the element kind: stays typed
a = [1, 2, 3]
a[1, 1] = IntPair.new(7, 8)
p a

# range form
b = [1, 2, 3, 4]
b[1..2] = IntPair.new(9, 9)
p b

# mixed to_ary elements widen the receiver, then splice faithfully
c = [1, 2, 3]
c[1, 1] = Tagged.new(5)
p c

# an object without to_ary inserts as one element (widening the receiver)
d = [1, 2, 3]
d[0, 2] = Opaque.new(1)
p d.length
p d[1]

# a poly receiver (array element that is an array at runtime) coerces too
e = [[1, 2, 3], "x"]
e[0][1, 1] = IntPair.new(4, 5)
p e[0]

# value position: the expression value is the object itself, not the coercion
f = [1, 2, 3]
r = (f[1, 1] = IntPair.new(6, 6))
p r.is_a?(IntPair)
p f
