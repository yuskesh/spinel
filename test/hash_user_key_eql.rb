# A Hash keyed by a custom object honors user-defined #hash / #eql?: an
# equal-but-distinct key finds the entry (sp_gen_obj_hash / sp_gen_obj_eql
# hooks). `self == o` inside #eql? dispatches to the user #== even though
# the operand is poly; `alias eql? ==` resolves to the target's C symbol.
class Point
  attr_reader :x, :y
  def initialize(x, y) = (@x, @y = x, y)
  def ==(o)   = o.is_a?(Point) && x == o.x && y == o.y
  def eql?(o) = self == o
  def hash    = [x, y].hash
end

grid = {}
grid[Point.new(1, 2)] = "alive"
p grid[Point.new(1, 2)]
p grid.key?(Point.new(1, 2))
p grid[Point.new(9, 9)]

a = Point.new(1, 2)
b = Point.new(1, 2)
c = Point.new(3, 4)
p a == b
p a != c
p a != b
boxed = [b, "x"]
p a == boxed[0]
p a != boxed[0]

class Pt2
  attr_reader :x
  def initialize(x) = (@x = x)
  def ==(o) = o.is_a?(Pt2) && x == o.x
  alias eql? ==
  def hash = x.hash
end
g = {}
g[Pt2.new(7)] = "yes"
p g[Pt2.new(7)]
p g.key?(Pt2.new(7))
