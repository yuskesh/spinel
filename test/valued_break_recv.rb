# Self-returning iterators with an IMPURE receiver: the receiver expression is
# evaluated exactly once (spilled to a temp used as both the loop source and
# the no-break result).
$mk = 0
def make_arr
  $mk += 1
  [1, 2, 3]
end

p(make_arr.each { |x| break 9 if x == 2 })
p $mk

# no break taken: the receiver value is the result, still evaluated once
$mk = 0
p(make_arr.each { |x| break 9 if x == 99 })
p $mk

# each_with_index / reverse_each variants
$mk = 0
p(make_arr.each_with_index { |x, i| break i if x == 2 })
p $mk
$mk = 0
p(make_arr.reverse_each { |x| break x if x == 2 })
p $mk

# an ivar-mutating receiver expression
class Counter
  attr_reader :n
  def initialize; @n = 0; end
  def bump_and_get
    @n += 1
    [@n, @n * 2]
  end
end
c = Counter.new
p(c.bump_and_get.each { |v| break v if v > 1 })
p c.n

# statement position with an impure receiver
$mk = 0
make_arr.each { |x| break if x == 2 }
p $mk
