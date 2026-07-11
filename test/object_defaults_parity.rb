# Object defaults on user classes: is_a?/kind_of? against universal
# ancestors, default to_s / inspect rendering, and clone(freeze:).

class Plain; end
class Sub < Plain; end

a = Plain.new
p a.is_a?(Object)
p a.kind_of?(Object)
p a.is_a?(BasicObject)
p a.is_a?(Kernel)
p a.instance_of?(Object)
p a.is_a?(Sub)
s = Sub.new
p s.is_a?(Plain)
p s.is_a?(Object)

# default to_s / inspect: value-shaped class (immutable scalar ivar)
class Widget
  def initialize = @id = 7
end
w = Widget.new
ws = w.to_s
wi = w.inspect
p ws.start_with?("#<Widget:0x")
p ws.end_with?(">")
p wi.start_with?("#<Widget:0x")
p wi.include?("@id=7")

# default to_s / inspect: heap class (mutable ivars)
class Counter
  def initialize = @n = 0
  def bump = @n += 1
end
cobj = Counter.new
cobj.bump
cs = cobj.to_s
cinsp = cobj.inspect
p cs.start_with?("#<Counter:0x")
p cinsp.include?("@n=1")

# clone with the freeze keyword
str = "hi"
p str.clone(freeze: false)
p str.clone(freeze: false).frozen?
p str.clone(freeze: true).frozen?
p str.clone
arr = [1, 2]
p arr.clone(freeze: true).frozen?
p arr.clone(freeze: false).frozen?
