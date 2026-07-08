# An anonymous positional splat (`def m(a, *) = f(a, *)`, Ruby 3.0) must forward
# through a receiver call, like a named `*args` does.
#
# An anonymous `*` was dropped def-side (only a named rest was registered), and
# the anonymous `*` at the call site is a SplatNode with no expression, so the
# argument packer emitted the -1 node and rejected ("no type"). Register the
# anonymous rest under a synthetic name and resolve the call-site `*` to it.

# 1. The gap: leading arg + anonymous rest forwarded to a rest-taking method.
class R
  def call(m, *args) = [m, args]
end
class W
  def initialize(r) = (@r = r)
  def invoke(method, *) = @r.call(method, *)
  def go = invoke(:a, 1, 2)
end
p W.new(R.new).go                  #=> [:a, [1, 2]]

# 2. Pure anonymous forward.
class Q
  def sum(*a) = a.sum
  def fwd(*) = sum(*)
end
p Q.new.fwd(1, 2, 3)               #=> 6

# 3. Leading arg + anon rest, empty and non-empty.
class Z
  def take(x, *r) = [x, r]
  def pass(y, *) = take(y, *)
end
p Z.new.pass(:k)                   #=> [:k, []]
p Z.new.pass(:k, 1, 2)             #=> [:k, [1, 2]]

# 4. A named splat is unaffected (regression).
class N
  def call(m, *a) = [m, a]
  def inv(method, *args) = call(method, *args)
end
p N.new.inv(:x, 9, 8)              #=> [:x, [9, 8]]

puts "done"
