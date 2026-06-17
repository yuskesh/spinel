# An @ivar read in a default-argument value, on a value-type receiver, must
# dereference the receiver with `.` (value struct), not `->`.
class C
  def initialize
    @r = "v"
  end
  def get_it(r = @r)
    r
  end
end
puts C.new.get_it
