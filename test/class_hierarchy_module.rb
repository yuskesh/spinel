# Issue #404 Phase 3 Tier 2. Modules in the unified cls_id
# space + woven into ancestors via the @cls_includes pass.
#
# Coverage:
#   - module constant in value position (Greeting / Comparable
#     lookalike) lowers to a sp_Class with the unified cls_id.
#   - <class>.ancestors weaves included modules in include-reverse
#     order followed by the parent chain.
#   - <class> <= <module> checks via the ancestors table.
#   - <class> == <module> never holds (different cls_ids).

module Greeting
  def hello
    "hello"
  end
end

module Politeness
  def please
    "please"
  end
end

class Base
end

class Polite < Base
  include Greeting
  include Politeness
end

# Module name as value -- compiles to sp_Class with unified cls_id.
m = Greeting
puts m.to_s          # => Greeting

# Ancestors include the modules in include-reverse order.
def names_of(arr)
  out = ""
  arr.each do |klass|
    out += "," unless out.length == 0
    out += klass.to_s
  end
  out
end

puts names_of(Polite.ancestors)
# => Polite,Politeness,Greeting,Base

# <= check on a module via the ancestors table.
puts (Polite <= Greeting) ? "polite<=Greeting" : "polite!<=Greeting"
puts (Polite <= Politeness) ? "polite<=Politeness" : "polite!<=Politeness"
puts (Base <= Greeting) ? "base<=Greeting" : "base!<=Greeting"

# Equality: a class is never == its included module.
puts (Polite == Greeting) ? "eq" : "neq"
