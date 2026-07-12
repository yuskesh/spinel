# A user class that mixes in Enumerable via a hand-written #each: its methods
# (map/select/to_a/sum, direct #each return value) and `for x in obj` all
# materialize through the synthesized element array. The bare-yield each
# returns the last yielded block value (not self), matching CRuby.
class Nums
  include Enumerable
  def initialize(*xs) = @xs = xs
  def each; @xs.each { |x| yield x }; end
end

nums = Nums.new(1, 2, 3, 4)
p nums.map { |x| x * 2 }
p nums.select(&:even?)
p nums.to_a
p nums.sum
p nums.min
p nums.include?(3)

# `for` over the user Enumerable
total = 0
for x in nums
  total += x
end
p total

def collect_for(e)
  out = []
  for v in e
    out << v * 10
  end
  out
end
p collect_for(Nums.new(5, 6))

# bare-yield each returns the last yielded block value (CRuby semantics)
class Y
  def each; yield 1; yield 2; yield 3; end
end
p(Y.new.each { |v| v * 100 })

# multi-assignment for over pair-yielding each
class Pairs
  include Enumerable
  def each; yield [1, :a]; yield [2, :b]; end
end
pairs = []
for k, v in Pairs.new
  pairs << "#{k}:#{v}"
end
p pairs
