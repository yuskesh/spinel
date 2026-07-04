# An explicit method overriding an inherited (or same-class) attr_reader/
# attr_accessor must run via normal dispatch, not be shadowed by direct field
# access at the call site. The more-derived definition wins.

# subclass def x overrides inherited attr_accessor reader
class Attr
  attr_accessor :x
  def initialize(v)
    @x = v
  end
end

class Child < Attr
  def x
    @x * 10
  end
end

puts Child.new(3).x            # 30

# subclass def x= overrides inherited attr_accessor writer
class ChildW < Attr
  def x=(v)
    @x = v * 10
  end
end

cw = ChildW.new(1)
cw.x = 5
puts cw.x                       # 50

# same-class def overrides its own attr_reader
class Same
  attr_reader :x
  def initialize(v)
    @x = v
  end
  def x
    @x + 100
  end
end

puts Same.new(3).x             # 103

# child attr_reader overrides parent's explicit def -> reads the field
class Parent
  def initialize(v)
    @x = v
  end
  def x
    @x + 100
  end
end

class ChildA < Parent
  attr_reader :x
end

puts ChildA.new(3).x           # 3

# plain attr with no override keeps direct field access
class Plain
  attr_accessor :x
  def initialize(v)
    @x = v
  end
end

p = Plain.new(7)
p.x = 9
puts p.x                        # 9
