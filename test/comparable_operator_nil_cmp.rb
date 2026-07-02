# Object <, <=, >, >=, ==, != and between? via a user <=> that can return nil:
# an incomparable pair raises the Comparable ArgumentError for the operators
# and between?, while == is false (and != true) without raising, and identity
# is always equal -- matching CRuby compar.c. A <=> that always returns an
# Integer keeps working (the fast inline path).
class Weight
  include Comparable
  attr_reader :g
  def initialize(g); @g = g; end
  # nil for a negative operand: forces a runtime-nil <=> between same-class
  # operands so the checked path is exercised
  def <=>(other); other.g < 0 ? nil : (g <=> other.g); end
end

class Money
  include Comparable
  attr_reader :cents
  def initialize(cents); @cents = cents; end
  def <=>(other); cents <=> other.cents; end
end

def w_id(x); x; end
def m_id(x); x; end

w1 = w_id(Weight.new(1))
w2 = w_id(Weight.new(2))
bad = w_id(Weight.new(-5))

# comparable pairs work
p w1 < w2
p w2 > w1
p w1 <= w1
p w2 >= w2
p w1.between?(w1, w2)
p w1 == w_id(Weight.new(1))
p w1 != w2

# incomparable operands raise for the operators...
begin
  w1 < bad
rescue ArgumentError => e
  puts e.message
end
begin
  w1 >= bad
rescue ArgumentError => e
  puts e.message
end
begin
  w2.between?(w1, bad)
rescue ArgumentError => e
  puts e.message
end

# ...but == is false (never raises), != is true, and identity always wins
p w1 == bad
p w1 != bad
p bad == bad

# an always-Integer <=> class keeps working
m1 = m_id(Money.new(10))
m2 = m_id(Money.new(20))
p m1 < m2
p m1.between?(m1, m2)
p m1 == m_id(Money.new(10))
