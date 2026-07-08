# Numeric coerce protocol: `n <op> obj` where obj defines coerce asks the
# object to coerce the pair, then applies the operator. The canonical form
# returns a pair of the object's own class and dispatches its operator.
class Money
  attr_reader :cents
  def initialize(c) = @cents = c
  def coerce(other) = [Money.new(other * 100), self]
  def +(o) = Money.new(@cents + o.cents)
  def to_s = "$%.2f" % (@cents / 100.0)
end
puts (Money.new(500) + Money.new(250)).to_s
puts (5 + Money.new(250)).to_s

class Vec
  attr_reader :x
  def initialize(x) = @x = x
  def coerce(other) = [Vec.new(other), self]
  def +(o) = Vec.new(@x + o.x)
  def *(o) = Vec.new(@x * o.x)
  def -(o) = Vec.new(@x - o.x)
  def to_s = "Vec(#{@x})"
end
puts (10 + Vec.new(5)).to_s
puts (3 * Vec.new(4)).to_s
puts (20 - Vec.new(8)).to_s
puts (2.5 + Vec.new(1.5)).to_s
