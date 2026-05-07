# `(@h[k] ||= []) << v` — chained `||=` over a Hash element with a
# trailing method call. The parenthesised subexpression evaluates
# to the (just-created or pre-existing) Array; `<<` then pushes
# into it.
#
# Pre-fix the IndexOrWriteNode expression-form fell through to the
# default catch-all and emitted literal `0` as the chain
# receiver — the resulting `sp_poly_shl(0, …)` failed C compile
# (`0` is `int`, sp_poly_shl wants `sp_RbVal`). Spinel now lowers
# `(h[k] ||= v)` into a get-then-set temp that the chain reads.

class C
  def initialize
    @h = {}                       # promotes to sym_poly_hash on first []=
  end
  def add(k, v)
    (@h[k] ||= []) << v
  end
  def count(k); @h[k].length; end
end

c = C.new
c.add(:a, 1)
c.add(:a, 2)
c.add(:b, 10)
c.add(:b, 20)
c.add(:b, 30)
puts c.count(:a)   # 2
puts c.count(:b)   # 3
