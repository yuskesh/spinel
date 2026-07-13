# Array/Comparable conformance (KieranP #2287,#2289-2294)
class Ver
  include Comparable
  attr_reader :n
  def initialize(n); @n = n; end
  def <=>(o); n <=> o.n; end
end
p(Ver.new(5).respond_to?(:<))       # #2287 Comparable-mixin operators
p(Ver.new(5).respond_to?(:between?))
p([] + [1])                         # #2289 empty array literal operand
p([1] + [])
p([] - [1])
p([1, "a", 2] - ["a"])              # #2290 different element types
p([1, 2] - [2, "a"])
p([1, 2] & [2, 3])
p([1, 2] | ["a"])
p([].push(1))                       # #2291 mutate empty literal directly
p([] << 5)
p([1, 2].unshift(nil))              # #2292 unshift nil into a numeric array
p([1.1, 2.2].unshift(nil))
p([1, 2].unshift(nil, nil))
p([1.1, 2.2].include?(2.2))         # #2293 Float array include?
p([1.1, 2.2].include?(9.9))
p([].nil?)                          # #2294 nil? on an empty array literal
p([].empty?)
