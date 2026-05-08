# 4-level chained `||=` mirroring optcarrot's PPU#setup_lut
# `@lut_update` shape:
#
#     (((@h[b] ||= [])[i] ||= [nil, nil])[0] ||= []) << v
#
# Levels 1, 2, 3 are all `||=` against poly-element receivers
# (poly_poly_hash, then a poly value carrying a poly_array twice).
# Pre-fix only level 1 was lowered; levels 2 and 3 fell through to
# the int default and the chain collapsed to `0 << ...`. The fix
# threads sp_RbVal temps through every level and promotes ArrayNode
# rhs literals (`[]`, `[nil, nil]`) to poly_array so each
# intermediate `cls_id == SP_BUILTIN_POLY_ARRAY` probe matches at
# runtime.

class C
  def initialize
    @h = {}
  end
  def push(b, i, v)
    (((@h[b] ||= [])[i] ||= [nil, nil])[0] ||= []) << v
  end
  def count(b, i); @h[b][i][0].length; end
end

c = C.new
c.push(0, 5, 10)
c.push(0, 5, 20)
c.push(0, 7, 30)
c.push(1, 0, 100)
puts c.count(0, 5)   # 2
puts c.count(0, 7)   # 1
puts c.count(1, 0)   # 1
