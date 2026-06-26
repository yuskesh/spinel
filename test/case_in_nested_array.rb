# A nested array pattern in `case/in` (`in [a, [b, c], d]`) destructures the
# sub-array element, binding its inner targets, and matches only when the
# element is itself a correctly-shaped array. Previously the nested element was
# skipped (inner targets bound nil) and matched on outer length alone.
# Each scrutinee routes through its own method param (so the runtime path runs);
# separate helpers keep each param type concrete and avoid cross-case widening.

# nested match
def n1(x); x; end
case n1([1, [2, 3], 4])
in [a, [b, c], d] then p [a, b, c, d]      # [1, 2, 3, 4]
end

# middle is the wrong length -> no match
def n2(x); x; end
case n2([1, [2], 4])
in [a, [b, c], d] then p :y
else p :no                                  # :no
end

# a typed int array can never match a nested-array pattern -> no match
def n3(x); x; end
case n3([1, 5, 4])
in [a, [b, c], d] then p :y
else p :no                                  # :no
end

# deeply nested
def n4(x); x; end
case n4([1, [2, [3, 4]]])
in [a, [b, [c, d]]] then p [a, b, c, d]     # [1, 2, 3, 4]
end

# nested + trailing rest
def n5(x); x; end
case n5([1, [2, 3], 4, 5])
in [a, [b, c], *r] then p [a, b, c, r]      # [1, 2, 3, [4, 5]]
end

# nested capture: bind the whole sub-array and its parts
def n6(x); x; end
case n6([1, [2, 3]])
in [a, [b, c] => pair] then p [a, b, c, pair]  # [1, 2, 3, [2, 3]]
end

# string sub-array as the first element
def n7(x); x; end
case n7([["a", "b"], 1])
in [[x, y], z] then p [x, y, z]             # ["a", "b", 1]
end

# flat pattern still works (regression)
def n8(x); x; end
case n8([1, "two", 3])
in [a, b, c] then p [a, b, c]               # [1, "two", 3]
end
