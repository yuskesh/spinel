# A yield-based method called with a block on a constant receiver. The constant
# holds an instance (not a class name), so the inline path must fall through to
# instance-method lookup rather than treating it as a class method -- otherwise
# the call referenced an unemitted function and failed to link.
class Bag
  def initialize(a)
    @a = a
  end
  def each
    @a.each { |x| yield x }
  end
  def sum_each
    s = 0
    @a.each { |x| s += x }
    s
  end
end

NUMS = Bag.new([1, 2, 3])
NUMS.each { |x| print x, " " }
puts
p NUMS.sum_each        # 6

PAIRS = Bag.new([[1, 2], [3, 4]])
PAIRS.each do |pair|
  pair.each { |y| print y, " " }
end
puts
