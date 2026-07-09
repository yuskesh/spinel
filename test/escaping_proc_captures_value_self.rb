# An escaping proc that reads its enclosing instance's ivars must capture self.
# For a value-type (by-value) class, self is captured by value in the proc's
# capture struct (a real sp_X field), not by pointer.

class Box
  def initialize(a, b)
    @a = a
    @b = b
  end
  def mk = proc { @a + @b }
end
p Box.new(3, 4).mk.call

# value-type with a String ivar (the captured self must be GC-scanned)
class Pt
  def initialize(name, x)
    @name = name
    @x = x
  end
  def label = proc { "#{@name}=#{@x}" }
end
p Pt.new("a", 5).label.call

# capture self AND a parameter local
class Calc
  def initialize(base)
    @base = base
  end
  def adder(k) = proc { @base + k }
end
p Calc.new(10).adder(3).call

# many escaping procs stored then called later
class Acc
  def initialize(v)
    @v = v
  end
  def getter = proc { @v * 2 }
end
p [Acc.new(1).getter, Acc.new(2).getter, Acc.new(3).getter].map(&:call)

# escaping proc CREATED INSIDE initialize: self is held as sp_X* there, so the
# body's by-value captured self must still read ivars with `.`, not `->`.
class Reg
  def initialize(n, sink)
    @n = n
    sink << proc { @n * 3 }
  end
end
sink = []
Reg.new(5, sink)
Reg.new(6, sink)
p sink.map(&:call)

# escaping Fiber created inside initialize of a value-type class -- the same
# by-value-self / `.`-deref requirement as the proc path.
class FibReg
  def initialize(v, fsink)
    @v = v
    fsink << Fiber.new { Fiber.yield(@v + 1); @v + 2 }
  end
end
fsink = []
FibReg.new(10, fsink)
fib = fsink[0]
p fib.resume
p fib.resume
