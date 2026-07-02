# Comparable#clamp parity for user classes: reversed bounds raise, nil bounds
# clamp one-sided, the range form accepts Integer endpoints (a clamped result
# IS that endpoint), an exclusive range raises, and a poly receiver routes
# through the user <=>. Receivers go through per-shape helper defs to defeat
# constant folding.
class Money
  include Comparable
  attr_reader :cents
  def initialize(cents); @cents = cents; end
  def <=>(other)
    v = other.is_a?(Money) ? other.cents : (other.is_a?(Integer) ? other : nil)
    v.nil? ? nil : (cents <=> v)
  end
end

def money_id(x); x; end
def poly_id(x); x; end
def int_id(x); x; end

# reversed bounds raise
begin
  money_id(Money.new(20)).clamp(Money.new(30), Money.new(10))
rescue ArgumentError => e
  puts e.message
end

# nil bounds clamp one-sided (the nil side is never returned, so the result
# keeps the receiver's class)
p money_id(Money.new(5)).clamp(Money.new(10), nil).cents
p money_id(Money.new(5)).clamp(nil, Money.new(3)).cents
p money_id(Money.new(5)).clamp(nil, Money.new(9)).cents

# range form with Integer endpoints: the user <=> sees the Integer; a clamped
# result is the Integer endpoint itself
p money_id(Money.new(0)).clamp(1..5)
p money_id(Money.new(9)).clamp(1..5)
# in-range returns the receiver (not an endpoint); clamped returns the endpoint
p money_id(Money.new(3)).clamp(1..5).is_a?(Integer)
p money_id(Money.new(0)).clamp(1..5).is_a?(Integer)

# beginless / endless ranges clamp one-sided
p money_id(Money.new(9)).clamp(..5)
p money_id(Money.new(0)).clamp(2..)

# an exclusive range with a real end cannot clamp; exclusive endless can
begin
  money_id(Money.new(3)).clamp(1...5)
rescue ArgumentError => e
  puts e.message
end
p money_id(Money.new(0)).clamp(2...)

# Integer receivers: exclusive range raises there too
begin
  int_id(7).clamp(1...5)
rescue ArgumentError => e
  puts e.message
end
p int_id(7).clamp(1..5)
p int_id(7).clamp(1..)
p int_id(0).clamp(..5)

# a poly receiver holding a user object clamps via the user <=>
p poly_id(Money.new(50)).clamp(Money.new(10), Money.new(30)).cents
p (poly_id(Money.new(50)).clamp(10..30))
begin
  poly_id(Money.new(3)).clamp(1...5)
rescue ArgumentError => e
  puts e.message
end
