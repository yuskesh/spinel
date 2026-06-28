# `super` inside initialize_copy whose only ancestor is Object: Object provides
# initialize_copy as a no-op copy hook, so super must not raise NoMethodError.
# clone/dup already deep-copies the struct before the user hook runs, so the
# original stays independent of the copy's later mutation.
class Board
  def initialize
    @table = [[1], [2]]
  end
  def initialize_copy(orig)
    super
    @table = @table.clone
  end
  def push_row
    @table.push([9])
  end
  def size
    @table.length
  end
end

b = Board.new
c = b.clone
c.push_row
puts c.size
puts b.size
