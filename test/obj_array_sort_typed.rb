# No-block sort/sort!/min/max over arrays of user objects. A local touched
# only by modeled ops narrows to the typed object-array representation (the
# runtime compares through the cmp hook); a component whose sort result
# escapes into an unmodeled consumer stays on the boxed poly path -- the
# behavior is identical either way. A class without <=> raises like CRuby.
class Money
  include Comparable
  attr_reader :cents
  def initialize(cents); @cents = cents; end
  def <=>(other); cents <=> other.cents; end
end

# narrowed: only admitted ops touch this local and its sort alias
bag = [Money.new(30), Money.new(10), Money.new(20)]
sorted = bag.sort
p sorted.first.cents
p sorted.last.cents
p bag.min.cents
p bag.max.cents
p bag.first.cents        # sort is a copy; the receiver is unchanged
bag.sort!
p bag.first.cents        # sort! reorders in place

# escaping consumer: the component stays poly (boxed hook path)
esc = [Money.new(3), Money.new(1), Money.new(2)]
p esc.sort.length

# empty array: min/max are nil
none = [Money.new(1)]
none.sort!
p none.min.nil?

# equal-comparing elements: the merge schedule is fixed (and matches CRuby's
# small-array ordering), so tied elements land in a deterministic order on
# every platform
class Card
  include Comparable
  attr_reader :rank, :tag
  def initialize(rank, tag); @rank = rank; @tag = tag; end
  def <=>(other); rank <=> other.rank; end
end
deck = [Card.new(1, 0), Card.new(0, 1), Card.new(1, 2), Card.new(0, 3), Card.new(1, 4)]
ordered = deck.sort
p ordered.map(&:tag)

# a class without <=> cannot compare, matching CRuby's error
class NoCmp
  attr_reader :v
  def initialize(v); @v = v; end
end
plain = [NoCmp.new(2), NoCmp.new(1)]
begin
  plain.sort
  puts "no raise"
rescue ArgumentError => e
  puts e.message
end
