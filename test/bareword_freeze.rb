# Bareword freeze (implicit self) inside an instance method sets the
# instance's GC-header frozen bit and returns self, so the defensive-freeze
# idiom carries real state (frozen? reads it back; mutation raises).
# Reads after freeze are unaffected.
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
