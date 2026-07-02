# Analysis-time widening: an index write or splice whose value a typed array
# cannot hold widens the local (or ivar) to a poly array, so the write stores
# the value exactly as CRuby does instead of rejecting (or miscompiling the
# single-index form). The receivers here are direct locals -- the static
# route, complementing the runtime-promotion route in array_splice.rb.

# single-index []= with a mismatched value (formerly emitted invalid C)
a = [1, 2]
a[0] = "s"
p a
b = [1, 2]
b[1] = nil
p b
f = [1.5, 2.5]
f[0] = "x"
p f

# splice with a mismatched-element array source
c = [1, 2, 3]
c[1, 1] = ["x"]
p c

# splice with a mismatched scalar source
d = [1, 2, 3]
d[0, 2] = "s"
p d

# range-form splice with a mixed source
e = [1, 2, 3]
e[1..2] = [nil, "y"]
p e

# nil-gap fill past the end now works on a (widened) direct local
g = [1, 2]
g[4, 0] = ["z"]
p g

# same-element-type splices keep the typed representation (and still splice)
h = [1, 2, 3, 4]
h[1, 2] = [9]
p h

# aliasing: the widened local and its alias see the same array
i = [1, 2]
j = i
i[0] = :sym
p j

# value position: the expression value is the RHS as written
k = [1, 2, 3]
p(k[1, 1] = "v")
p k

# ivar route: a typed ivar widened by a mismatched splice in another method
class Holder
  def initialize
    @xs = [1, 2, 3]
  end
  def poke
    @xs[1, 1] = ["mid"]
  end
  def dump
    @xs
  end
end
hh = Holder.new
hh.poke
p hh.dump

# the alias fix also covers push widening (previously read garbage through j)
pi = [1, 2]
pj = pi
pi << "s"
p pj
