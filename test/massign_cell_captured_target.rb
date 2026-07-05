# Multi-assign targets captured by a later lambda live in heap cells, so the
# target write must go through the cell (doom's draw_automap min_x/max_y,
# closed over by the to_sx/to_sy procs, were written as undeclared lv_ names).
def bounds(a)
  min_x, max_x, min_y, max_y = a[0], a[1], a[2], a[3]
  to_sx = ->(wx) { wx - min_x }
  to_sy = ->(wy) { max_y - wy }
  puts to_sx.call(10)
  puts to_sy.call(3)
  puts max_x - min_y
  min_x, max_y = 100, 200
  puts to_sx.call(10)
  puts to_sy.call(3)
end
bounds([1, 2, 3, 4])
