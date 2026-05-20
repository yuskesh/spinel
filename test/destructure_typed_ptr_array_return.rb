# Top-level `y, z = call_returning_pair_of_objs` segfaulted pre-fix:
# collect_scoped_multi_const pushed every top-level MultiWriteNode
# into @multi_const_inits, even when no target was a constant. The
# codegen loop then planted a pre-body `_t = call(lv_a, lv_b);` emit
# before the locals it reads from had been assigned. With both
# args still NULL the inlined method body deref'd NULL and crashed.
# Issue #630.

class Mat
  attr_accessor :nrows, :ncols
  def initialize(nrows, ncols)
    @nrows = nrows
    @ncols = ncols
  end
  def matmul(other)
    Mat.new(@nrows, other.ncols)
  end
end

def fwd(w, x)
  y = w.matmul(x)
  z = w.matmul(x)
  [y, z]
end

w = Mat.new(3, 4)
x = Mat.new(4, 1)
y, z = fwd(w, x)
puts y.nrows
puts z.ncols
