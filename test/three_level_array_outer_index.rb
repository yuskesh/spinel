# `[a, b, c, d].map { (0...M).map { (0..N).map { ... } } }` builds a
# 3-level Array<Array<IntArray>>. The outer .map's receiver is an
# array literal; spinel's compile_map_expr stored that receiver in a
# C-stack temp without registering it with the GC. When the block
# body's nested allocations crossed the GC threshold (~272 inner
# rows triggered the first sp_gc_collect), the unrooted receiver was
# freed mid-loop and the next iteration's `sp_IntArray_length(rc)`
# read the len of whatever IntArray reused that heap slot — usually
# the most recent inner (0..N).map result, length N+1.
#
# Symptom on master: `TBL.length` printed 8 instead of 4, and every
# `TBL[X]` returned the same recycled inner row regardless of X.
# The fix roots the receiver temp when its static type is a
# GC-allocated pointer.
#
# A second related fix in compile_bracket_assign covers
# `recv[start, len] = src` where recv is statically poly_array and
# src is poly (e.g. `@bg_pixels[xfine, 8] = @bg_pattern_lut[idx]`
# when bg_pattern_lut comes from a poly_array<poly_array<int_array>>
# constant). Without the dispatch the slice assignment silently
# became `recv[start] = len` and dropped the rhs entirely.

# Inner range is sized large enough (>= ~272) that an inner GC
# triggers during the outer .map's iteration. Using runtime-bound
# indices (i, j, k) to read TBL[a][b][c] avoids spinel emitting the
# `int >> literal_index & 1` bit-index warning branch (spinel
# can't statically tell that TBL[a] is always an array, so it
# generates both branches; the int-branch shift count >= 64
# triggers -Werror in the test runner with literal indices).
TBL = [10, 20, 30, 40].map do |a|
  (0...500).map do |i|
    (0..7).map do |j|
      a + i + j
    end
  end
end

puts TBL.length          # 4
puts TBL[0].length       # 500

a0 = 0
a1 = 1
a2 = 2
a3 = 3
i0 = 0
i499 = 499
j0 = 0
j7 = 7

puts TBL[a0].length        # 500
puts TBL[a0][i0].length    # 8
puts TBL[a0][i0][j0]       # 10
puts TBL[a1][i0][j0]       # 20
puts TBL[a3][i0][j7]       # 47
puts TBL[a2][i499][j0]     # 529
puts TBL[a3][i499][j7]     # 546

# Slice assign with poly_array recv + poly src — the
# `@buf[start, len] = @table[a][i]` shape that @bg_pixels falls
# into in optcarrot once TILE_LUT is no longer split.  @table is
# poly_array<poly_array<int_array>>, so @table[a][i] is `poly`
# (sp_RbVal).  @buf is sized via `[nil] * N` so it's poly_array
# (sp_PolyArray *).  Without the dispatch fix the bracket-assign
# falls through to single-elem write `@buf[start] = len`,
# silently dropping the rhs.
class Slicer
  def initialize
    @table = [10, 20, 30].map { |a| (0...4).map { |i| (0..3).map { |j| a + i + j } } }
    @buf = [nil] * 7
    @buf[0] = -1; @buf[1] = -1; @buf[2] = -1; @buf[3] = -1
    @buf[4] = -1; @buf[5] = -1; @buf[6] = -1
  end
  def fill(a, i); @buf[1, 4] = @table[a][i]; end
  def get(k); @buf[k]; end
end

s = Slicer.new
s.fill(2, 1)
puts s.get(0)            # -1
puts s.get(1)            # 31
puts s.get(2)            # 32
puts s.get(3)            # 33
puts s.get(4)            # 34
puts s.get(5)            # -1
s.fill(0, 0)
puts s.get(1)            # 10
puts s.get(4)            # 13
