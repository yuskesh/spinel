# tap / then / yield_self at method-tail position carry their value out as
# the return: tap returns the (mutated) receiver, then returns the block's
# value. Struct member writes inside the tap block mutate the same instance.
Point = Struct.new(:x, :y)

def build = Point.new(1, 2).tap { |pt| pt.x = 9 }
p build.x
p build.y

def shifted(p0) = p0.tap { |pt| pt.y = pt.y + 10 }
q = Point.new(3, 4)
p shifted(q).y
p q.y

def doubled(n) = n.then { |v| v * 2 }
p doubled(21)

def renamed(s) = s.yield_self { |v| v + "!" }
p renamed("hi")

def chained = Point.new(5, 6).tap { |pt| pt.x = 7 }.tap { |pt| pt.y = 8 }
r = chained
p [r.x, r.y]

mid = Point.new(0, 0).tap { |pt| pt.x = 4 }
p mid.x
