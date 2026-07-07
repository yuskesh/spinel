# A user object with #hash + #eql? works as a Hash key: two value-equal keys
# collide to the same slot and compare equal, so a freshly-built key finds the
# stored value. Previously object keys used pointer identity and always missed.

class K
  attr_reader :v
  def initialize(v) = (@v = v)
  def hash = v.hash
  def eql?(other) = other.is_a?(K) && other.v == v
end

h = {}
h[K.new(1)] = "a"
h[K.new(2)] = "b"
p h[K.new(1)]        # "a" — value-equal fresh key
p h[K.new(2)]        # "b"
p h[K.new(3)]        # nil — absent
h[K.new(1)] = "z"    # overwrite the same logical key
p h[K.new(1)]        # "z"
p h.size             # 2

# a string-backed key
class Name
  attr_reader :s
  def initialize(s) = (@s = s)
  def hash = s.hash
  def eql?(o) = o.is_a?(Name) && o.s == s
end
n = {}
n[Name.new("bob")] = 10
n[Name.new("amy")] = 20
p n[Name.new("bob")]
p n[Name.new("zzz")]

# a second key class coexists (exercises the cls_id switch arms)
class Pt
  attr_reader :x, :y
  def initialize(x, y) = (@x, @y = x, y)
  def hash = [x, y].hash
  def eql?(o) = o.is_a?(Pt) && o.x == x && o.y == y
end
g = {}
g[Pt.new(1, 2)] = "p"
p g[Pt.new(1, 2)]
p g[Pt.new(2, 1)]

# primitive #hash is an Integer and stable within a run
p 5.hash.is_a?(Integer)
p("x".hash == "x".hash)
p([1, 2].hash == [1, 2].hash)
