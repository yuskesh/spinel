# Singleton-class blocks define methods on an object's singleton class. The
# `class << self` form inside a class/module body already worked; this adds the
# `class << Const` form (a constant naming a class or module) and the top-level
# `class << self` form. Both were rejected ("unsupported ... SingletonClassNode")
# because the inner defs were attached to the enclosing scope's class rather
# than the named one, and a top-level singleton-class node had no codegen path.

# class << Const on a module
module Greeter; end
class << Greeter
  def hello; "hello"; end
end
p Greeter.hello

# class << Const on a class, with arguments
class Calc; end
class << Calc
  def add(a, b); a + b; end
end
p Calc.add(3, 4)

# multiple defs and class-level ivar state on the singleton
module Registry; end
class << Registry
  def push(x); @items ||= []; @items << x; @items; end
  def count; @items ? @items.length : 0; end
end
Registry.push(:a)
Registry.push(:b)
p Registry.count

# class << self inside a class body still works (regression)
class Config
  class << self
    def version; "1.0"; end
  end
end
p Config.version

# top-level class << self defines a private-ish helper on main
class << self
  def helper(n); n * 2; end
end
p helper(21)
