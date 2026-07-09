# Array-pattern bindings against a poly/untyped VALUE scrutinee must be boxed
# (poly), not int. A helper widened to an untyped return by extra call sites
# used to type the rest slice / required element as int and reinterpret the
# boxed element bits -> garbage output (ryanseys/spinel#418).

def poly(a) = a

# widen poly()'s inferred element union to include a nested-array type so the
# return type collapses to untyped (the trigger for the original miscompile).
_z1 = poly([1, 2, [3, 4]])
_z2 = poly([1, 2, 5])

# rest slice: pre must be a poly array of the leading elements.
case poly([1, "x", 3, 4])
in [*pre, last]; p [pre, last]; end          #=> [[1, "x", 3], 4]

# required head + rest: first must keep its (boxed) value, rest the remainder.
case poly(["x", 1, 2, 3])
in [first, *rest]; p [first, rest]; end       #=> ["x", [1, 2, 3]]

# a middle required between splats (FindPattern) on the same poly helper.
case poly([1, "a", 2, "b", 3])
in [*, mid, *]; p mid; end                    #=> 1  (first non-greedy middle)

# regression: a Data value-type object still binds its members (non-splat).
D = Data.define(:x, :y)
case D.new(5, 6)
in [a, b]; p [a, b]; end                      #=> [5, 6]
