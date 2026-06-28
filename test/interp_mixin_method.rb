# Interpolating a method whose type is known only through the including module:
# the module method is analyzed with an unknown self, so the call types as
# unknown; codegen re-resolves it against the class being emitted.
module Greeting
  def greet; "hi #{name}, you are #{age}"; end   # name -> String, age -> Integer
end

class Person
  include Greeting
  def initialize(n, a); @n = n; @a = a; end
  def name; @n; end
  def age; @a; end
end

puts Person.new("bob", 42).greet
puts Person.new("amy", 7).greet
