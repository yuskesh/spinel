# Bare `super` (implicit argument forwarding) inside a `block_given?` branch.
# When a method body references block_given?, calling it with a block inlines
# the body and renames the param locals for the capture context; bare super's
# forwarded arguments must reference the renamed locals, not the original names.
class Base
  def m(x = nil); "base(#{x})"; end
  def two(a, b); "base(#{a},#{b})"; end
end

class Sub < Base
  def m(x = nil)
    if block_given? then super else "self" end
  end
  def two(a, b)
    if block_given? then super else "self" end
  end
end

# with a block: inlined, params renamed -> super forwards the renamed locals
p Sub.new.m(1) { 0 }
p Sub.new.two(1, 2) { 0 }
# without a block: the non-inlined path still works
p Sub.new.m(7)
p Sub.new.two(3, 4)

# a poly-typed forwarded argument (boxing branch) in the same context
class Wrap
  def m(x); "wrap:#{x.inspect}"; end
end
class WrapSub < Wrap
  def m(x); block_given? ? super : "self"; end
end
def val(v); v; end
p WrapSub.new.m(val("hi")) { 0 }

# prepend chain: a prepended method referencing block_given? is inlined at a
# block call site and its params renamed; the bare super to the prepended-over
# method must forward the renamed locals too (a sibling of the parent-chain
# forwarding above).
module Logged
  def compute(x)
    if block_given? then super else "noblock" end
  end
end
class Calc
  prepend Logged
  def compute(x); "calc(#{x})"; end
end
p Calc.new.compute(5) { 0 }
p Calc.new.compute(7)
