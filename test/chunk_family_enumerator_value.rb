# chunk_while / slice_when / chunk with a block, standing on their own
# (stored in a local, passed to p) rather than chained into .to_a: a
# first-class Enumerator over the eagerly materialized runs, inspecting as
# the Generator wrapper CRuby shows (address normalized below). Custom
# Enumerable classes (redirected through __enum_to_a) and typed int/str
# array receivers are all served; .to_a terminal chains keep their arms.
class Nums
  include Enumerable
  def initialize(*xs); @xs = xs; end
  def each; @xs.each { |x| yield x }; end
end
norm = ->(s) { s.sub(/0x[0-9a-f]+/, "0xADDR") }
e = Nums.new(1, 2, 4, 5).chunk_while { |a, b| b - a == 1 }
puts norm.call(e.inspect)
p e.to_a
s = [1, 2, 4, 5].slice_when { |a, b| b - a > 1 }
puts norm.call(s.inspect)
p s.to_a
i = [3, 4, 7].chunk_while { |a, b| b == a + 1 }
puts norm.call(i.inspect)
p i.to_a
w = %w[aa ab ba].chunk_while { |a, b| a[0] == b[0] }
p w.to_a
p([1, 2, 4, 5].chunk_while { |a, b| b - a == 1 }.to_a)
