# A `break <v>` inside a block makes the iterator call return <v> (CRuby's
# TAG_BREAK). It works across value-producing iterators (map/select/reduce/...)
# and self-returning ones (each/each_with_index). Bare `break` returns nil; a
# `break a, b` returns an array. Each receiver is a typed local (monomorphic, so
# it is not widened to poly) to exercise the runtime path rather than folding.
arr = [1, 2, 3]

# collect family: result is the break value, not a partial array
p arr.map { |x| break 99 if x == 2; x * 10 }
p arr.select { |x| break :stop if x == 2; x.odd? }
p arr.reject { |x| break 7 if x == 2; false }
p arr.filter_map { |x| break "f" if x == 2; x }
p arr.flat_map { |x| break [0] if x == 2; [x, x] }

# reduce / find / count / *_by
p arr.reduce(0) { |s, x| break 100 if x == 2; s + x }
p arr.find { |x| break(-1) if x == 2; false }
p arr.count { |x| break 5 if x == 2; true }
p arr.sort_by { |x| break :s if x == 2; x }
p arr.min_by { |x| break 0 if x == 2; x }

# range and integer iterators
rng = (1..5)
n = 3
m = 1
p rng.map { |x| break 42 if x == 3; x }
p n.times { |x| break :done if x == 1 }
p m.upto(4) { |x| break x * 100 if x == 3 }

# self-returning iterators: break replaces the receiver result
p arr.each { |x| break 9 if x == 2 }
labelled = [10, 20, 30]
p labelled.each_with_index { |x, i| break i if x == 20 }
big = (1..9)
p big.each { |x| break x * x if x == 4 }
p arr.each_with_object([]) { |x, o| break :bail if x == 2; o << x }

# no break: the iterators still return their normal value
p arr.map { |x| x + 1 }
p arr.reduce(0) { |s, x| s + x }

# bare break -> nil; break with several values -> array
p arr.each { |x| break if x == 2 }
p arr.map { |x| break 1, 2, 3 if x == 2; x }

# break binds to the nearest enclosing block: an inner each's break does not
# escape the outer each
total = 0
inner = [10, 20]
arr.each do |x|
  inner.each { |y| total += y; break }
  total += x
end
p total

# a break inside a nested while binds to the while, not the iterator
p arr.map { |x| k = 0; while true; k += 1; break if k == 2; end; x + k }

# multi-param destructuring with break
pairs = [[1, 2], [3, 4]]
p pairs.map { |a, b| break "stop" if b == 4; a + b }
