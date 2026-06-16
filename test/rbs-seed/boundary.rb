# Regression for #1417 boundary coercion. Once seeds apply, a method's RBS
# return/param type can be narrower than its poly body value or call-site arg,
# so codegen must unbox the poly at the boundary:
#   - `Slots.get` is RBS `-> String?` but reads a poly hash value (return path);
#     the missing-key case must round-trip nil -> NULL -> nil.
#   - `Use.label` takes an RBS `Thing` but is called with a poly array element
#     (argument path).
# Without the coercion the generated C assigns sp_RbVal to a const char* /
# sp_Thing* slot and fails to compile.
module Outer
  class Thing
    def initialize(n)
      @n = n
    end
    def n
      @n
    end
  end
  module Slots
    @h = {}
    def self.set(k, v)
      @h[k] = v
    end
    def self.get(k)
      @h[k]
    end
  end
  module Use
    def self.label(t)
      t.n
    end
  end
end

Outer::Slots.set(:x, "hi")
puts Outer::Slots.get(:x)
puts Outer::Slots.get(:y).nil?
puts Outer::Use.label([Outer::Thing.new("z")][0])
