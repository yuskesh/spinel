# `Klass.class_eval do ... end` reopens the class: each `def` and
# `define_method(:lit)` in the block body becomes an instance method on
# the class, exactly as a `class Klass ... end` reopen would. A def may
# read an existing ivar or assign a new one (which gets its own slot),
# subclasses inherit the added methods, and `module_eval` is an alias.
# The receiver may be a constant (plain or namespaced `M::Klass`) or, inside
# a class body, a bare/`self.` receiver naming the enclosing class.
class Box
  def initialize(v)
    @v = v
  end
end

class BoxPlus < Box
end

Box.class_eval do
  def doubled
    @v * 2
  end

  define_method(:tripled) do
    @v * 3
  end

  # A String literal names the method just as a Symbol does.
  define_method("quadded") do
    @v * 4
  end
end

# module_eval is the same operation under a different name.
Box.module_eval do
  def labelled
    @label = "n=" + @v.to_s
    @label
  end
end

b = Box.new(21)
puts b.doubled
puts b.tripled
puts b.quadded
puts b.labelled

# A subclass inherits the class_eval-added instance methods.
bp = BoxPlus.new(5)
puts bp.doubled
puts bp.tripled

# A namespaced receiver (`M::Widget`) reopens the same way.
module M
  class Widget
    def initialize(v)
      @v = v
    end
  end
end

M::Widget.class_eval do
  def quad
    @v * 4
  end
end

puts M::Widget.new(3).quad

# Inside a class body, a bare or `self.` receiver names the enclosing class.
class Gadget
  def initialize(v)
    @v = v
  end

  class_eval do
    def a
      @v
    end
  end

  self.class_eval do
    def doubled
      @v * 2
    end
  end
end

g = Gadget.new(10)
puts g.a
puts g.doubled
