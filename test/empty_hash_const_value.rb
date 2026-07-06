# An empty hash literal as a receiver ({}.freeze) types as the same str-keyed
# poly hash codegen emits for a bare {} -- without it the constant was dropped
# from codegen entirely (reads raised "uninitialized constant", and a typed
# ivar's `|| CONST` fallback broke the C build).
class C
  EMPTY = {}.freeze
  def initialize
    @h = nil
  end
  def read
    @h || EMPTY
  end
end
p C.new.read
p C::EMPTY.empty?
p C::EMPTY.size
TOP = {}.freeze
p TOP
