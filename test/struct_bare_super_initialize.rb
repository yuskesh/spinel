# A bare `super` (no parens) inside a Struct's custom initialize forwards the
# method's own params into the members -- it must not leave them nil.
Pair = Struct.new(:a, :b) do
  def initialize(a, b)
    super
  end
end
p = Pair.new(3, 4)
puts [p.a, p.b].inspect

# Bare super still forwards when the params are recomputed before the call and
# when member types are mixed (int + string).
Rec = Struct.new(:n, :label) do
  def initialize(n, label)
    n = n * 10
    super
  end
end
r = Rec.new(2, "hi")
puts [r.n, r.label].inspect

# Fewer forwarded params than members: the rest stay nil (Struct semantics).
Trip = Struct.new(:x, :y, :z) do
  def initialize(x, y)
    super
  end
end
t = Trip.new(1, 2)
puts [t.x, t.y, t.z].inspect
