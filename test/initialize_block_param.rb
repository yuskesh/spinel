# An explicit `&blk` block parameter on initialize is threaded through .new:
# the constructor accepts the block and forwards it, so a stored block can be
# called later.
class Widget
  def initialize(&blk)
    @cb = blk
  end
  def fire = @cb.call(10)
end
puts Widget.new { |x| x * 3 }.fire

# block combined with a positional argument
class Handler
  def initialize(name, &cb)
    @name = name
    @cb = cb
  end
  def run(x) = "#{@name}: #{@cb.call(x)}"
end
puts Handler.new("double") { |n| n * 2 }.run(21)

# optional block: absent -> nil ivar
class Maybe
  def initialize(&blk)
    @blk = blk
  end
  def has? = !@blk.nil?
end
puts Maybe.new { }.has?
puts Maybe.new.has?
