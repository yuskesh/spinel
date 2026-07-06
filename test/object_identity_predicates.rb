# Object#equal? is reference identity (a heap instance is its pointer);
# freeze is a no-op returning self, so frozen? consistently reports false.
class Foo
  def initialize(v)
    @v = v
  end
end
a = Foo.new(1)
b = a
c = Foo.new(1)
p a.equal?(b)
p a.equal?(c)
p a.equal?(a)
p c.freeze.equal?(c)
p 1.equal?(1)
