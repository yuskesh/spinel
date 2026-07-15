# Class-object `.new` dispatch (zero-arg) only builds classes whose initialize
# takes no required args; an arg-requiring constructor is omitted (its runtime
# cls_id lands in the nil default, matching MRI's ArgumentError), and a
# value-type class is boxed by value not as a heap pointer (#2450).
class A
  def initialize; @v = "a"; end
  def v; @v; end
end
class Other
  def initialize(x); @x = x; end
  def x; @x; end
end
class Rel
  def initialize(model); @model = model; end
  def build; @model.new; end
end
puts Rel.new(A).build.v
puts Other.new("keep").x
