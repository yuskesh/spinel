# `...` forwarding for positional arguments (#1288): the compiler synthesizes
# concrete params from the call sites and forwards them directly -- no rest
# array / splat. Free function, instance method, class method, multiple sites.
def target(a, b, c)
  a + b + c
end
def fwd(...)
  target(...)
end
puts fwd(1, 2, 3)
puts fwd(10, 20, 30)

def join2(a, b)
  "#{a}-#{b}"
end
def wrap(...)
  join2(...)
end
puts wrap("x", "y")

class Calc
  def add(a, b)
    a + b
  end
  def run(...)
    add(...)
  end
  def self.mul(a, b)
    a * b
  end
  def self.build(...)
    mul(...)
  end
end
puts Calc.new.run(3, 4)
puts Calc.build(3, 4)

# keyword arguments forward too (keyword params compile to positional C params
# mapped by name, so synthesizing a key-named param carries them).
def kw_target(a:, b:)
  a * 10 + b
end
def kw_fwd(...)
  kw_target(...)
end
puts kw_fwd(a: 3, b: 4)

def mixed(x, label:)
  "#{label}=#{x}"
end
def mixed_fwd(...)
  mixed(...)
end
puts mixed_fwd(7, label: "n")

# chained forwarding: a forwarding method called only via another forward
# infers its arity transitively from its target (h -> f -> g).
def leaf(a)
  a * 2
end
def mid(...)
  leaf(...)
end
def top(...)
  mid(...)
end
puts top(21)
