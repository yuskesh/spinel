# freeze state on Proc/Enumerator; is_a? for included builtin modules
# (KieranP #2362, #2363)
a = ->(x) { x }
p a.frozen?
a.freeze
p a.frozen?
p a.freeze.equal?(a)
e = [1].each
p e.frozen?
e.freeze
p e.frozen?
class Ver
  include Comparable
  attr_reader :n
  def initialize(n); @n = n; end
  def <=>(o); n <=> o.n; end
end
p Ver.new(5).is_a?(Comparable)
class Sub < Ver; end
p Sub.new(1).is_a?(Comparable)     # inherited include
class Nums
  include Enumerable
  def each; yield 1; end
end
p Nums.new.is_a?(Enumerable)
class Plain; end
p Plain.new.is_a?(Comparable)
module M; end
class WithM; include M; end
p WithM.new.is_a?(M)
