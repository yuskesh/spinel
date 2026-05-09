# Cross-type equality between a primitive and a user-class, both directions.

# Box -- is_a? short-circuits, so only the typed-arg compile path
# (under -Werror) and the primitive-vs-obj fold get exercised.
class Box
  attr_reader :v
  def initialize(v); @v = v; end
  def ==(other)
    return false unless other.is_a?(Box)
    @v == other.v
  end
end

b = Box.new(5)
ms = String.new("x") # mutable_str — distinct codegen type from string literal

# Cross-type pairs -> false (each primitive, both directions).
puts b == 5
puts b == 1.5
puts b == "x"
puts b == ms
puts b == :foo
puts b == true
puts b == false
puts b == nil
puts 5 == b
puts 1.5 == b
puts "x" == b
puts ms == b
puts :foo == b
puts true == b
puts false == b
puts nil == b

# Same pairs negated -> true.
puts b != 5
puts b != 1.5
puts b != "x"
puts b != ms
puts b != :foo
puts b != true
puts b != false
puts b != nil
puts 5 != b
puts 1.5 != b
puts "x" != b
puts ms != b
puts :foo != b
puts true != b
puts false != b
puts nil != b

# Wrapper -- ==(other) reads `other`, so the boxing round-trip is
# exercised end-to-end (not just the is_a? short-circuit).
class Wrapper
  def initialize(v); @v = v; end
  def ==(other); @v == other; end
end

# Same type, same value -> true.
puts Wrapper.new(5)       == 5
puts Wrapper.new(1.5)     == 1.5
puts Wrapper.new("hello") == "hello"
puts Wrapper.new(:foo)    == :foo
puts Wrapper.new(true)    == true
puts Wrapper.new(false)   == false
puts Wrapper.new(nil)     == nil

# Same type, different value -> false.
puts Wrapper.new(5)       == 6
puts Wrapper.new(1.5)     == 2.5
puts Wrapper.new("hello") == "world"
puts Wrapper.new(:foo)    == :bar
puts Wrapper.new(true)    == false

# Reverse direction stays primitive-strict (MRI semantics).
puts 5       == Wrapper.new(5)
puts "hello" == Wrapper.new("hello")

# != synthesis through the value-flow path.
puts Wrapper.new(5) != 5
puts Wrapper.new(5) != 6

puts "done"
