# Array#clone aliases Array#dup for poly (nested) arrays: a shallow top-level
# copy independent of the original. Covers a bare poly array and the clone of a
# poly-array instance variable inside initialize_copy (the regressing shape).
def via(x); x; end

a = [[1], [2], [3]]
b = via(a).clone
b << [4]
puts a.length        # 3 -- appending to the clone leaves the original alone
puts b.length        # 4

class Board
  def initialize; @table = [[1], [2]]; end
  def initialize_copy(orig); @table = @table.clone; end
  def push(row); @table.push(row); end
  def size; @table.length; end
end

orig = Board.new
copy = orig.clone
copy.push([9])
puts orig.size       # 2 -- initialize_copy cloned @table, so the original is intact
puts copy.size       # 3
