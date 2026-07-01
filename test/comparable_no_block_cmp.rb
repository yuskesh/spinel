# No-block sort/min/max/clamp over a user class that mixes in Comparable must
# dispatch the user `<=>` (not the built-in poly comparator). A nil `<=>` result
# (incomparable operands) raises ArgumentError, matching CRuby. Receivers are
# routed through a method param so the calls hit the runtime comparator path.
class Money
  include Comparable
  attr_reader :cents
  def initialize(cents); @cents = cents; end
  def <=>(other); cents <=> other.cents; end
end

# distinct monomorphic helpers per receiver shape (a shared one would go poly)
def bag_id(x); x; end
def money_id(x); x; end
def wbag_id(x); x; end

bag = bag_id([Money.new(30), Money.new(10), Money.new(20)])

p bag.sort.map(&:cents)
p bag.min.cents
p bag.max.cents

# clamp keeps the receiver's class (returns self or a bound)
p money_id(Money.new(50)).clamp(Money.new(10), Money.new(30)).cents
p money_id(Money.new(5)).clamp(Money.new(10), Money.new(30)).cents
p money_id(Money.new(20)).clamp(Money.new(10), Money.new(30)).cents

# a `<=>` that yields nil for foreign operands makes the comparison raise
class Weight
  include Comparable
  attr_reader :g
  def initialize(g); @g = g; end
  def <=>(other); other.is_a?(Weight) ? (g <=> other.g) : nil; end
end

begin
  wbag_id([Weight.new(3), "kg", Weight.new(1)]).sort
  puts "no raise"
rescue ArgumentError => e
  puts e.message
end

# the recorded pair must be deterministic (libc-independent): exercise the
# foreign operand at every position of 2- and 3-element arrays; the receiver
# in the message is whichever operand the fixed merge schedule compares first
def try_sort(a)
  begin
    a.sort
    puts "no raise"
  rescue ArgumentError => e
    puts e.message
  end
end

try_sort(["kg", Weight.new(1)])
try_sort([Weight.new(1), "kg"])
try_sort(["kg", Weight.new(1), Weight.new(2)])
try_sort([Weight.new(1), Weight.new(2), "kg"])

# minmax (no block) via the user <=>; min(n)/max(n) route through the sorted
# order; Array#<=> compares object arrays elementwise
p bag.minmax.map(&:cents)
p bag.min(2).map(&:cents)
p bag.max(2).map(&:cents)
p (bag <=> bag_id([Money.new(30), Money.new(10), Money.new(20)]))
p (bag <=> bag_id([Money.new(30), Money.new(99)]))
empty = bag_id([])
p empty.minmax
