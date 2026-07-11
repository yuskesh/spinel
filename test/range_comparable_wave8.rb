# Range: char-range variables, beginless/endless membership, min(n)/max(n),
# take/drop/reverse_each/filter_map, % (step), clamp(range) incl. variables,
# and Comparable between?/clamp on a custom class.
cr = ('a'..'e'); p(cr.to_a)
p((..5).include?(3))
p((..5).include?(7))
p((6..).include?(7))
p((1..10).min(3))
p((1..10).max(3))
p((1..5).take(3))
p((1..3).reverse_each.to_a)
p((1..5).filter_map { |x| x * 10 if x.odd? })
r = (((1..10) % 3).to_a rescue "no each"); p r
rv = (1..10); r2 = (15.clamp(rv) rescue "no clamp"); p r2
p(0.clamp(rv))
p(5.clamp(rv))
p((1..10).min(3))
p((1..10).max(3))
p((1..5).take(3))
p((1..5).drop(3))
p((1..3).reverse_each.to_a)
p((1..5).filter_map { |x| x * 10 if x.odd? })
r = (((1..10) % 3).to_a rescue "no each"); p r
p(((0..20) % 5).to_a)
p((..5).include?(3))
p((..5).include?(7))
p((6..).include?(7))
p((6..).include?(2))
p((..5).cover?(5))
p((...5).include?(5))
p((1..3).include?(2))
p((3..1).include?(2))
rv = (1..10)
r = (15.clamp(rv) rescue "no clamp"); p r
p(0.clamp(rv))
p(5.clamp(rv))
p(15.clamp(1..10))
cr = ('a'..'e'); p(cr.to_a)
p(cr.include?('c'))
p(cr.map { |ch| ch.upcase })
class Ver139
  include Comparable
  attr_reader :n
  def initialize(n); @n = n; end
  def <=>(o); n <=> o.n; end
end
p(Ver139.new(5).between?(Ver139.new(1), Ver139.new(9)))
p(Ver139.new(0).between?(Ver139.new(1), Ver139.new(9)))
v = Ver139.new(12).clamp(Ver139.new(1)..Ver139.new(9))
p v.n
w = Ver139.new(5).clamp(Ver139.new(1), Ver139.new(9))
p w.n
