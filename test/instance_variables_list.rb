# Object#instance_variables lists the object's ivar names as symbols
# (the class layout is static, so the list is compile-time known).
class C
  def initialize
    @a = 1
    @b = 2
  end
end
p C.new.instance_variables
c = C.new
p c.instance_variables
p c.instance_variable_get(:@a)
c.instance_variable_set(:@a, 9)
p c.instance_variable_get(:@a)
class Empty; end
p Empty.new.instance_variables
