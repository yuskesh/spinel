class Ver139
  include Comparable
  attr_reader :n
  def initialize(n); @n = n; end
  def <=>(o); n <=> o.n; end
end
lo = Ver139.new(1); hi = Ver139.new(9)
p(Ver139.new(5).clamp(lo..hi).n)
p(Ver139.new(0).clamp(lo..hi).n)
p(Ver139.new(12).clamp(lo, hi).n)
# value obj crossing into a poly slot and back out via GC pressure
class Tag139
  attr_reader :label, :num
  def initialize(label, num); @label = label; @num = num; end
end
box = []
10.times { |i| box << Tag139.new("t#{i}", i) }
20000.times { |i| "waste#{i}" * 3 }
GC.start if defined?(GC)
p box.map { |t| t.label }.join(",")
p box.last.num
