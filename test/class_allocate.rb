class Thing
  def initialize(x) = (@x = x)
  def x = @x
end

# allocate takes no args even though initialize requires one: it does not run
# initialize. The object is still a usable instance of the class.
t = Thing.allocate
puts t.class.name
puts t.is_a?(Thing)

# .new still constructs normally (initialize runs)
n = Thing.new(5)
puts n.x
puts n.class.name

# a bare instance is populated afterward: state assigned post-allocate reads
# back normally (allocate's purpose is to build an instance you initialize by
# hand). Reading before assignment is intentionally avoided.
class Point
  attr_accessor :x, :y
end
p = Point.allocate
p.x = 3
p.y = 4
puts p.x + p.y

# a nested (ConstantPath) class receiver resolves too
module Outer
  class Inner
    def initialize = (@v = 99)
    def v = @v
  end
end
i = Outer::Inner.allocate
puts i.class.name
puts Outer::Inner.new.v
