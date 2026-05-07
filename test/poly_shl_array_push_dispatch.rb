# `<<` on a poly recv used to always lower to sp_poly_shl, which
# was Integer-bit-shift only. An IntArray boxed into a poly slot
# (e.g. an ivar that the type-inference passes widened to poly
# because it received both nil and an array) had its `<<` push
# silently turn into a bit-shift of the encoded pointer/length,
# dropping the rhs.
#
# After the fix, sp_poly_shl dispatches by recv cls_id and invokes
# the matching Array#<< push. Falls through to bit-shift only when
# the recv is a non-array (genuine Integer<<int).

class C
  def initialize
    # `nil` then concrete-array writes widen @v to poly (Issue #130
    # "definite int/nil + obj → poly"). Once poly, the runtime
    # carries cls_id INT_ARRAY at access time.
    @v = nil
    @v = [10, 20, 30]
  end
  def push(x); @v << x; end
  def length; @v.length; end
  def get(i); @v[i]; end
end

c = C.new
c.push(99)
c.push(88)
puts c.length         # 5
puts c.get(0)         # 10
puts c.get(3)         # 99
puts c.get(4)         # 88

# Bit-shift on genuine Integer recv still works (the fall-through).
x = 1
puts x << 3           # 8
