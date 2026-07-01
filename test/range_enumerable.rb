# Enumerable methods on a Range that spinel does not handle natively are served
# by materializing the range to an int array and dispatching the array method:
# the non-collecting forms (reduce(:sym)/group_by/find/count/zip/tally). The
# range is a local so the materialization runs at runtime.
r = (1..5)

# reduce / inject with a symbol or an initial value
p r.reduce(:+)
p r.inject(:*)
p r.reduce(100, :+)
p r.inject(2, :*)

# group_by returns a Hash of arrays
p (1..6).group_by { |x| x % 3 }

# find / detect return the first matching element (or nil)
p r.find { |x| x > 3 }
p r.detect(&:even?)
p r.find { |x| x > 99 }

# count with a block or an argument (bare count stays Range#size)
p r.count(&:even?)
p r.count(3)
p r.count

# zip pairs with other collections
p (1..3).zip([4, 5, 6])
p (1..3).zip([4, 5, 6], [7, 8, 9])

# tally counts occurrences
p (1..3).tally

# exclusive ranges exercise the `last - excl` materialization bound
x = (1...5)
p x.reduce(:+)
p x.group_by { |n| n % 2 }
p x.count(&:even?)
p x.zip([10, 20, 30, 40])

# empty ranges (first > last) materialize to an empty int array. reduce with an
# initial value returns that value; the no-init `reduce(:sym)` form on an empty
# array is the pre-existing typed-array limitation (returns the element-type
# identity, not nil, since the result is a non-nullable int) and is not tested.
e = (5..1)
p e.reduce(100, :+)
p e.group_by { |n| n }
p e.find { |n| n > 0 }
p e.count(&:even?)
p e.tally

# zip with a shorter argument pads the tail with nil
p (1..3).zip([4, 5])

# the natively-handled range methods are unchanged
p r.map { |x| x * 2 }
p r.select(&:odd?)
p r.sum
p r.to_a
