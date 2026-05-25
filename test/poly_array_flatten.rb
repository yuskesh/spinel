# Issue #739. Array#flatten on poly_array (deep nested) and on the
# typed-array-of-int_array shape (uniform `[[a,b],[c,d]]` form).
# Both used to fall through to the unresolved-call warning and
# then segfault.

# Deep recursive nesting -- poly_array recv. The runtime walker
# recurses into IntArray / StrArray / PolyArray elements.
puts [[1, [2, [3]]]].flatten.inspect

# Heterogeneous mix of scalar + array elements.
puts [1, "two", [3, [:four, 5.0]]].flatten.inspect

# Uniform [[int...]] (typed-array-of-int_array) flattens to int_array.
puts [[1, 2], [3, 4], [5]].flatten.inspect

# Empty poly_array.
puts [].flatten.inspect
