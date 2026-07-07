# A `yield` / `block_given?` inside `initialize` must run the constructor body
# (and drive the block when one is passed), for both heap and value-type classes.
#
# The emitted constructor sp_X_new skips a yielding initialize (a yielding method
# is normally realized by inlining at its call sites, but new->initialize is a
# bespoke path). With no block at the call site, nothing inlined the body either,
# so every `@ivar = ...` in initialize vanished and the object came back with all
# ivars nil -- and a later method call on an ivar-held object segfaulted.
#
# Fix: inline the yielding initialize body at the construction site even when no
# block is given (block_given? folds to false, a guarded yield is dead code), and
# make that inline value-type aware (sp_X by value + "." deref, vs sp_X * + "->").

# 1. Heap class, constructed WITHOUT a block: ivars must be set (the gap).
class Grid
  attr_reader :width, :height
  def initialize(w, h = 10)
    @width = w
    @height = h
    yield self if block_given?
  end
end
g = Grid.new(4)
p g.width                          #=> 4
p g.height                         #=> 10

# 2. Heap class, constructed WITH a block: the block runs and sees self.
seen = nil
g2 = Grid.new(5) { |gg| seen = gg.width }
p g2.width                         #=> 5
p seen                             #=> 5

# 3. block_given? false path leaves the guarded yield unreached.
g3 = Grid.new(7, 8)
p [g3.width, g3.height]            #=> [7, 8]

# 4. Value-type class with a yielding initialize, WITH a block.
class Vec
  attr_reader :x, :y
  def initialize(x, y)
    @x = x
    @y = y
    yield self if block_given?
  end
end
sum = 0
v = Vec.new(3, 4) { |vv| sum = vv.x + vv.y }
p [v.x, v.y]                       #=> [3, 4]
p sum                              #=> 7

# 5. Value-type class constructed WITHOUT a block.
v2 = Vec.new(1, 2)
p [v2.x, v2.y]                     #=> [1, 2]

# 6. A method call on an ivar-held constructed object (the segfault case).
class Holder
  def initialize(n)
    @grid = Grid.new(n)
    yield if block_given?
  end
  def grid_width = @grid.width
end
p Holder.new(9).grid_width         #=> 9

puts "done"
