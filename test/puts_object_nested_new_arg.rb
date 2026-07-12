# puts/print of an object expression whose evaluation constructs nested
# instances: the argument's hoisted declarations must land as complete
# statements before the result temp's declaration.
class Money
  attr_reader :c
  def initialize(c) = @c = c
  def +(o) = Money.new(c + o.c)
  def to_s = "$#{c}"
end

m = Money.new(5)
puts(m + Money.new(3))
puts(Money.new(1) + Money.new(2) + Money.new(4))
print(m + Money.new(10))
print("\n")
puts "total: #{m + Money.new(7)}"
