# `(arr[idx] ||= []) << v` — chained `||=` over a poly_array
# element. Pre-fix the IndexOrWriteNode expression-form returned
# the int default for non-hash receivers, so the chain receiver
# collapsed to literal `0` and `<<` failed C compile.
# Spinel now lowers the get-then-set into an sp_RbVal temp that
# the chain reads — same shape as the *_poly_hash arms but with
# in-bounds + auto-grow handling for the indexed access.

class C
  def initialize
    @a = Array.new(3) { [] }    # promotes to poly_array of empty poly_arrays
  end
  def push(idx, v)
    (@a[idx] ||= []) << v
  end
  def count(i); @a[i].length; end
end

c = C.new
c.push(0, 10)
c.push(0, 20)
c.push(1, 100)
puts c.count(0)   # 2
puts c.count(1)   # 1
puts c.count(2)   # 0
