# to_enum / enum_for build a real lazy Enumerator that defers `recv.m(*args)`:
# the receiver's method is driven through a fiber generator, so .class is
# Enumerator, .next/.peek do external iteration, and the full Enumerable surface
# works. Multi-value yields (`yield a, b`) collapse to an array element, matching
# CRuby's ary2sv. Covers user classes (collection-backed, pure-yield,
# value-type), the `return enum_for(:each) unless block_given?` idiom, and
# builtin receivers.

# --- the canonical idiom: return enum_for(:each) unless block_given? ---
class Bag
  include Enumerable
  def initialize(*xs) = @xs = xs
  def each
    return enum_for(:each) unless block_given?
    @xs.each { |x| yield x }
  end
end

b = Bag.new(1, 2, 3)
p b.each.class
p b.each.to_a
p b.each.map { |x| x * 10 }
p b.each.select { |x| x > 1 }
puts b.each.count
puts b.each.include?(2)
e = b.each
p e.next
p e.next

# --- a pure-yield method (no backing collection) ---
class Seq
  def go
    return enum_for(:go) unless block_given?
    yield 1
    yield 2
    yield 3
  end
end
p Seq.new.go.to_a
p Seq.new.go.map { |x| x + 100 }
g = Seq.new.go
p g.next
p g.peek
p g.next

# --- multi-value yields collapse to array elements (ary2sv) ---
class Grid
  include Enumerable
  def initialize = @cells = [[1, 2], [3, 4]]
  def each
    return enum_for(:each) unless block_given?
    @cells.each { |a, b| yield a, b }
  end
end
p Grid.new.each.to_a
p Grid.new.each.map { |a, b| a + b }
p Grid.new.each.next

# --- to_enum on an arbitrary method name (separated form) ---
class Steps
  def initialize(n) = @n = n
  def upto_n
    i = 1
    while i <= @n
      yield i
      i += 1
    end
  end
  def stepper = to_enum(:upto_n)
end
p Steps.new(4).stepper.to_a
p Steps.new(4).stepper.select { |n| n.even? }

# --- builtin receivers ---
p [10, 20, 30].to_enum(:each).to_a
p({ "a" => 1, "b" => 2 }.to_enum(:each).to_a)
p "hi".to_enum(:each_char).to_a
p (1..3).to_enum(:each).to_a
p [5, 6, 7].to_enum(:each_with_index).to_a
