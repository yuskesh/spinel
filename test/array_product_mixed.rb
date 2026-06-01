# Array#product with a single array argument of a different element type
# yields a poly_array of [recv_elem, arg_elem] pairs. Two int arrays keep
# the homogeneous result.
p [1, 2].product(["a", "b"])
p [1, 2, 3].product(["x"])
p ["a", "b"].product([1, 2])
p [1, 2].product([3, 4])
p [1.5, 2.5].product([1, 2])
