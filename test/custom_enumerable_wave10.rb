class Nums
  include Enumerable
  def initialize(*xs); @xs = xs; end
  def each; @xs.each { |x| yield x }; end
end
p Nums.new(3, 1, 2).entries
n = Nums.new(3, 1, 2)
p(n.sum { |x| x * 2 })
p Nums.new(1, 2, 3, 4).each_cons(2).to_a
p Nums.new(1, 2, 3, 4, 5).each_slice(2).to_a
p Nums.new(1, 2, 4, 5).chunk_while { |a, b| b - a == 1 }.to_a
p Nums.new(3, 1, 2).lazy.map { |x| x * 2 }.first(2)
p Nums.new(3, 1, 2).minmax_by { |x| x }
Nums.new(1, 2, 3).cycle(2) { |x| p x }
p Nums.new(1, 1, 2, 3, 3).chunk { |x| x }.to_a
p Nums.new(1, 2, 4, 5).slice_when { |a, b| b - a > 1 }.to_a
# blockless map / range no-block enumerator chains
m = [1, 2, 3].map
p m.size
p m.class
p m
p([1, 2, 3].map.with_index { |x, i| [x, i] })
p((1..5).each_with_index.to_a)
p((1..3).cycle.first(7))
p((1..3).map.with_index { |x, i| [x, i] })
p((1..6).each_slice(2))
r19 = (2..4)
p r19.each_with_index.to_a
