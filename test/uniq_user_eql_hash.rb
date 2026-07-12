# Array#uniq dedups by the user-defined eql?/hash (like CRuby), not by identity;
# a class without eql? still dedups by identity, and == is unaffected.
class P
  attr_reader :n
  def initialize(n); @n = n; end
  def eql?(o); o.is_a?(P) && n == o.n; end
  def hash; n.hash; end
end
p [P.new(1), P.new(1)].uniq.size
p [P.new(1), P.new(2), P.new(1)].uniq.size
p [P.new(1), P.new(1)].uniq.map(&:n)
class Q
  def initialize(n); @n = n; end
end
p [Q.new(1), Q.new(1)].uniq.size
a = P.new(5)
p [a, a].uniq.size
p(P.new(1) == P.new(1))
