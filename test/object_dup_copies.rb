o = Object.new
p(o.dup == o)
p(o.dup.equal?(o))
class C
  def initialize
    @v = 1
  end
end
c = C.new
p(c.dup == c)
p(c.clone == c)
