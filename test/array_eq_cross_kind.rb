# Array == compares by value across storage kinds: a splat-rest rebuilds its
# arguments as a poly array, while the literal it is compared against may box
# as an int array. Ruby has one Array; storage kinds must not leak into ==.
pr = proc do |*a, b|
  [a, b]
end
p pr.call(1, 2, 3) == [[1, 2], 3]
p pr.call("x", "y") == [["x"], "y"]
p pr.call(1) == [[], 1]
p pr.call(1, 2) == [[1], 999]
