# A class that includes Enumerable and defines only #each gets the bare
# Enumerable methods (map/select/to_a/sort/count/...) directly, with no explicit
# enum_for: the compiler redirects each such call through the materialized array.
class IntBag
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

class WordBag
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

b = IntBag.new
b.add(3).add(1).add(2)

p b.map { |n| n * 10 }
p b.select { |n| n > 1 }
p b.reject { |n| n > 1 }
p b.to_a
p b.sort
p b.find { |n| n > 1 }
puts b.count
puts b.include?(2)
puts b.sum
puts b.min
puts b.max
puts b.any? { |n| n > 2 }
puts b.all? { |n| n > 0 }
puts b.one? { |n| n > 2 }
p b.take_while { |n| n > 2 }
p b.drop_while { |n| n > 2 }
p b.zip([10, 20, 30])
p b.grep(1..2)
p b.grep_v(1..2)

w = WordBag.new
w.add("hi").add("there").add("world")
p w.map { |s| s.upcase }
p w.select { |s| s.length > 2 }
p w.sort
puts w.count
