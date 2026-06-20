# Virtual dispatch on a poly receiver into a method whose parameter is unused
# (so its type never resolves) and is passed nil. The unused param is poly in
# the callee signature; the dispatch arg temp must be a boxed poly, not a `void`
# temp. Regression: `void _t = 0;` -> C compile error.
class Base
  def hook(a, b)
    "base:#{a}"
  end
end
class Child < Base
  def hook(a, b)
    "child:#{a}"
  end
end
def run(x)
  x.hook("p", nil)
end
[Base.new, Child.new].each { |it| puts run(it) }

# also exercise a second unused poly param fed a poly value
class Sink
  def take(x, y, z)
    "#{x}"
  end
end
class Sink2 < Sink
  def take(x, y, z)
    "2:#{x}"
  end
end
vals = ["a", 1]              # poly elements
def drive(s, v)
  s.take("hi", v, nil)
end
[Sink.new, Sink2.new].each { |s| puts drive(s, vals[0]) }

# self-dispatch: a method forwards to a self method that subclasses override,
# with an unused nil-fed param (the tep proxy hook shape). Exercises the
# object/self-receiver dispatch path (vs the poly-receiver one above).
class Proxy
  def forward
    before_forward("req", nil)
  end
  def before_forward(a, b)
    "base:#{a}"
  end
end
class GuardProxy < Proxy
  def before_forward(a, b)
    "guard:#{a}"
  end
end
[Proxy.new, GuardProxy.new].each { |p| puts p.forward }
