# A flat multi-param block over an array of arrays auto-splats each element
# across the params (CRuby block semantics) in every element-loop emitter,
# not just each/map. Regression: filter_map/count/any?/all?/one?/find/
# take_while/drop_while/partition bound the first param to the whole element.
DIRS = [[10, 1], [20, 2], [30, 3]]

puts DIRS.filter_map { |a, b| a + b if a > 15 }.inspect
puts DIRS.count { |a, b| a > 15 }
puts DIRS.any? { |a, b| b > 2 }
puts DIRS.all? { |a, b| b > 0 }
puts DIRS.none? { |a, b| b > 9 }
puts DIRS.one? { |a, b| a == 20 }
puts DIRS.find { |a, b| a > 15 }.inspect
puts DIRS.take_while { |a, b| a < 25 }.inspect
puts DIRS.drop_while { |a, b| a < 25 }.inspect
puts DIRS.partition { |a, b| a > 15 }.inspect
puts DIRS.select { |a, b| a > 15 }.inspect
puts DIRS.reject { |a, b| a > 15 }.inspect
puts DIRS.min_by { |a, b| -a }.inspect
puts DIRS.sort_by { |a, b| -a }.inspect
puts DIRS.flat_map { |a, b| [a, b] }.inspect

# group_by buckets keyed by a runtime-computed bool: exercises both the
# autosplat and boxed-bool key equality (Hash#inspect spacing differs across
# CRuby versions, so print the buckets instead)
g = DIRS.group_by { |a, b| b.odd? }
puts g[true].inspect
puts g[false].inspect

# a bare `next` mid-block filters the element out (the #1876 GoL shape:
# neighbours = DIRECTIONS.filter_map with bounds checks + hash read)
cells = {}
5.times { |x| cells[x.to_s] = x * 10 }
picked = [[-1, 0], [1, 0], [9, 9]].filter_map do |dx, dy|
  nx = 2 + dx
  if nx < 0 || nx >= 5
    next # out of bounds
  end
  cells[nx.to_s]
end
puts picked.inspect

# boxed bool as a hash key must be equal to itself across separate boxings
h = {}
h[1.odd?] = 1
h[3.odd?] = h[1.odd?] + 10
h[2.odd?] = 100
puts h.size
puts h[true]
puts h[false]
