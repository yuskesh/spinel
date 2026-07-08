# Bareword freeze (implicit self) inside an instance method is a no-op that
# returns self. spinel has no per-object frozen state, so this is behaviorally
# faithful for the common defensive-freeze idiom; frozen? (which would need
# that state) stays unsupported rather than reporting a wrong value.
class Config
  def initialize(v)
    @v = v
    freeze
  end
  def v = @v
end
puts Config.new(42).v

# freeze as an expression returns self, typed as the instance so it chains
class Box
  def initialize
    @items = [1, 2, 3]
  end
  def seal = freeze
  def items = @items
end
puts Box.new.seal.items.inspect
