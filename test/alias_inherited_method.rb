# `alias` can reference a method inherited from an ancestor, not just one
# defined directly on the class. The alias snapshots the inherited body.
class Parent
  def foo; "parent-foo"; end
  def greet(name); "hi " + name; end
end

class Child < Parent
  alias bar foo
  alias hail greet
end

puts Child.new.bar
puts Child.new.hail("ada")

# Two levels up resolves too.
class Grandchild < Child
  alias baz foo
end
puts Grandchild.new.baz
