# `nil | W` where W would otherwise be a value-layout class: any nil
# witness on a W-typed slot disqualifies the value layout (a by-value
# struct has no nil), falling back to the pointer form whose NULL-nil
# machinery narrows as usual. Each shape below was a miscompile
# (`return 0` into a struct return) or would be.
class W
  def initialize(v); @v = v; end
  def year; @v; end
end

# return nil | W
def maybe(s)
  return nil if s.nil?
  W.new(70)
end
a = maybe("x")
puts a.nil? ? "nil" : a.year.to_s
puts maybe(nil).nil?

# ternary arm nil
def pick(f)
  f ? W.new(1) : nil
end
p pick(true).nil?
p pick(false).nil?

# local seeded nil then assigned
w = nil
w = W.new(3) if maybe("x")
puts w.nil? ? "nil" : w.year.to_s

# optional param defaulting nil
def show(x = nil)
  x.nil? ? "none" : "got " + x.year.to_s
end
puts show
puts show(W.new(9))
