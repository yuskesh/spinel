# Nested yield-method inlining: the outer inlined body calls another yielding
# method on a DIFFERENT receiver, passing its own (renamed) locals as args,
# and the caller's block reads the caller's ivars. The args must respect the
# outer inline's rename table and the spliced block body must resolve `self`
# to the CALLER, not the inner receiver (doom's compute_slide reading @map
# inside a block passed through each_nearby_linedef -> each_linedef_near).
class Grid
  def initialize(cells)
    @cells = cells
  end

  def each_cell_near(lo, hi)
    @cells.each { |c| yield c if c >= lo && c <= hi }
  end
end

class Walker
  def initialize(grid, names)
    @grid = grid
    @names = names
  end

  def each_named(a, b)
    unless @names.empty?
      min_v = a < b ? a : b
      max_v = a < b ? b : a
      @grid.each_cell_near(min_v, max_v) { |c| yield c }
      return
    end
    @grid.each_cell_near(a, b) { |c| yield c }
  end

  def collect(a, b)
    out = []
    each_named(a, b) do |c|
      return out if c == 9
      out << @names[c % @names.size]
    end
    out
  end
end

g = Grid.new([2, 9, 4, 1, 7])
w = Walker.new(g, ["red", "green", "blue"])
puts w.collect(4, 1).inspect
puts w.collect(1, 9).inspect
