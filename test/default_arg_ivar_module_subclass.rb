# A default-argument @ivar read, where the method is mixed in via a module and
# called on a subclass, requires the subclass struct to inherit the ivar field
# (the parent only gained it from the transplanted module method).
module M
  def set_it
    @__r = "v"
  end
  def get_it(r = @__r)
    r
  end
end
class Base2
  include M
end
class Child < Base2
end
c = Child.new
c.set_it
puts c.get_it
