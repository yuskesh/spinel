# A class that includes Enumerable and defines each by forwarding an explicit
# &block (rather than using yield) still derives the Enumerable method set:
# each drives the synthesized materialization the same way a yield-based each
# does.
class Bag
  include Enumerable
  def initialize(*items) = @items = items
  def each(&blk) = @items.each(&blk)
end
b = Bag.new(3, 1, 2)
p b.map { |x| x * 2 }
p b.select { |x| x > 1 }
p b.reject { |x| x > 1 }
p b.find { |x| x > 1 }
p b.sort
p b.sort_by { |x| -x }
p b.include?(2)
p b.to_a
p b.min
p b.max
p b.count
p b.first
p b.any? { |x| x > 2 }
p b.all? { |x| x > 0 }
