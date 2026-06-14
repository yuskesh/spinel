# Array#transpose on an untyped nested array (poly_array of rows) swaps
# rows and columns, preserving each element's runtime type. The
# statically-typed nested arrays are specialized inline; this exercises
# the poly fallback (sp_poly_array_transpose) for arrays built
# dynamically or returned from a method.

ints = []
ints.push([1, 2, 3])
ints.push([4, 5, 6])
p ints.transpose

floats = []
floats.push([1.0, 2.0])
floats.push([3.0, 4.0])
p floats.transpose

strs = []
strs.push(["a", "b"])
strs.push(["c", "d"])
p strs.transpose

# transpose of a poly nested array returned from a method
def grid
  g = []
  g.push([10, 20, 30])
  g.push([40, 50, 60])
  g
end
p grid.transpose

# single row
one = []
one.push([7, 8, 9])
p one.transpose
