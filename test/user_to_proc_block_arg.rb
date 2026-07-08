# `&obj` where obj defines #to_proc converts through to_proc, exactly as CRuby
# does: to_proc is called ONCE to obtain the block, then that proc runs per element.
class Dbl
  def to_proc = ->(x) { x * 2 }
end
p [1, 2, 3].map(&Dbl.new)

# a converter held in a local variable
d = Dbl.new
p [10, 20].map(&d)

# forwarding to other iterators
class IsEven
  def to_proc = ->(x) { x.even? }
end
p [1, 2, 3, 4, 5, 6].select(&IsEven.new)
p [1, 2, 3, 4].count(&IsEven.new)

# to_proc is evaluated exactly once even with an observable side effect
class Counting
  def initialize; @calls = 0; end
  def to_proc; @calls += 1; ->(x) { x + @calls }; end
  def calls = @calls
end
c = Counting.new
p [0, 0, 0].map(&c)
p c.calls
