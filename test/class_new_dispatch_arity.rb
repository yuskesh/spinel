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

# an optional-arg constructor is zero-arg-compatible: its arm fills the
# default rather than calling the C function with too few arguments (#2452)
class WithOpt
  def initialize(attrs = {}); @attrs = attrs; end
  def size; @attrs.length; end
end
class WithOpt2
  def initialize(attrs = {}); @attrs = attrs; end
  def size; @attrs.length + 1; end
end
class Maker
  def initialize(model); @model = model; end
  def build; @model.new; end
end
puts Maker.new(WithOpt).build.size
puts Maker.new(WithOpt2).build.size
