# freeze on a user-defined VALUE-type instance (a small immutable scalar-ivar
# class represented by value, sp_X, not a heap pointer). Stateful freeze reaches
# for the object's GC-header bit, which a value struct does not carry; the mono
# arm must fall back to the self-returning no-op instead of passing the struct
# to sp_gc_freeze(void*). freeze compiles, returns self, and the object stays
# usable; frozen? on a value object reads false but must still evaluate its
# receiver so a side-effecting receiver expression is not discarded.
class Point
  attr_reader :x, :y

  def initialize(x, y)
    @x = x
    @y = y
  end
end

pt = Point.new(3, 4)
q = pt.freeze
puts q.x
puts q.y
puts pt.x

$calls = 0
def make_point
  $calls += 1
  Point.new(5, 6)
end

puts make_point.frozen?
puts $calls
