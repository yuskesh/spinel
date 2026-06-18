module Greet
  def hi = "hi"
  def shout(x) = x.upcase
end

class C
  include Greet
  def hi = "[" + super + "]"          # bare super into the module method
  def shout(x) = "<" + super + ">"    # forwards x up to the module method
end

puts C.new.hi
puts C.new.shout("yo")

# a class that includes the module but does NOT override stays on the normal
# transplant path (the module method is callable directly)
class D
  include Greet
end
puts D.new.hi

# multiple modules defining the same method: a later include takes precedence,
# and super chains through them in MRO order (C -> M2 -> M1)
module M1
  def tag = "M1"
end
module M2
  def tag = "M2(" + super + ")"
end
class F
  include M1
  include M2
  def tag = "F[" + super + "]"
end
puts F.new.tag

# super with no superclass method anywhere raises NoMethodError at runtime
# (CRuby raises when the method is called, not at definition)
class E
  def orphan = super
end
begin
  E.new.orphan
rescue => e
  puts "#{e.class}: #{e.message}"
end
