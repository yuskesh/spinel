# enum_for(:each) / to_enum materializes a user class's #each into an array, so
# the full Enumerable surface (to_a/map/select/count/...) works on a class that
# only defines #each. The compiler synthesizes a per-class helper that drives
# the (lowered) #each with a collector block.
class Bag
  include Enumerable

  def initialize
    @items = []
  end

  def add(x)
    @items << x
    self
  end

  def each
    @items.each { |x| yield x }
  end
end

class Counter
  def initialize(n)
    @n = n
  end

  def each
    i = 1
    while i <= @n
      yield i
      i += 1
    end
  end
end

b = Bag.new
b.add("apple").add("banana").add("cherry")

p b.enum_for(:each).to_a
p b.to_enum.to_a
p b.enum_for(:each).map { |s| s.upcase }
p b.to_enum.select { |s| s.length > 5 }
puts b.enum_for(:each).count
puts b.enum_for(:each).include?("banana")

c = Counter.new(4)
p c.enum_for(:each).to_a
p c.to_enum.map { |n| n * n }
puts c.enum_for(:each).select { |n| n.even? }.length
