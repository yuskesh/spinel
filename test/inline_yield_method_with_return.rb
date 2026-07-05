# A yielding method containing `return` must still inline at block call sites
# (a bailed inline emitted a plain call to a never-emitted symbol, an
# undefined-symbol link error: doom's PlayerPhysics#each_nearby_linedef).
# The method's own returns funnel to a per-inline exit; a `return` inside the
# caller's block still exits the CALLER.
class Walker
  def initialize(items)
    @items = items
    @fast = false
  end

  def each_item(a, b)
    unless @fast
      @items.each { |it| yield it }
      return
    end
    @items.each { |it| yield it if it >= a && it <= b }
  end

  def count_between(a, b)
    n = 0
    each_item(a, b) do |it|
      return -1 if it < 0
      n += 1
    end
    n
  end
end

w = Walker.new([3, 1, 4, 1, 5])
puts w.count_between(1, 4)
w2 = Walker.new([3, -2, 9])
puts w2.count_between(0, 10)
