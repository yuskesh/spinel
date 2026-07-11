# Hash#fetch_values with a block for missing keys, Hash#delete with a block,
# hash subset/superset comparison operators, and Enumerable none?/one?/find_all
# on a hash receiver (plus plain-array find_all).
h = { a: 1, b: 2 }
p h.fetch_values(:a, :b)
p h.fetch_values(:a, :z) { |k| "missing #{k}" }

d = { a: 1, b: 2 }
p d.delete(:b)
p d.delete(:z) { |k| "gone #{k}" }
p d

small = { a: 1 }
big = { a: 1, b: 2 }
same = { a: 1, b: 2 }
p small < big
p small <= big
p big > small
p big >= small
p same < big
p same <= big
p big < small
p({ a: 2 } <= big)

n = { a: 1, b: 2 }
p n.none? { |k, v| v > 5 }
p n.none? { |k, v| v > 1 }
p n.one? { |k, v| v > 1 }
p n.one? { |k, v| v > 0 }
p n.find_all { |k, v| v > 1 }
p n.find_all { |k, v| v > 5 }

p [1, 2, 3].find_all { |x| x > 1 }
p [1, 2, 3].find_all(&:even?)
