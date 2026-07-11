# A yielding method is inlined at its block call site. The inlined body
# runs in the RECEIVER's class context, so an implicit-self call inside
# it (`to_a` / `size` below, both defined on Relation) must resolve
# against Relation — not against the CALLER's class (Paginator), which
# has no such method. Regression: the inline bound `self` to the
# receiver but left the emitting-class context as the caller's.
class Relation
  def initialize(items)
    @items = items
  end

  def to_a
    @items
  end

  def size
    @items.length
  end

  # yields; also makes two implicit-self calls (to_a, size)
  def each_doubled
    to_a.each { |x| yield x * 2 }
    size
  end
end

class Paginator
  def run
    r = Relation.new([1, 2, 3])
    total = 0
    n = r.each_doubled { |v| total += v }
    [total, n]
  end
end

p Paginator.new.run
