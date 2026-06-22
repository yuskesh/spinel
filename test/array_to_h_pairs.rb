# Array#to_h (pair form) builds a hash from [key, value] sub-arrays. Each pair
# may itself be a homogeneous typed array (IntArray for [1, 2], StrArray for
# ["a", "b"]) or a mixed PolyArray, so the element extraction must dispatch on
# the pair's own array kind rather than assuming PolyArray.
p [[1, 2], [3, 4]].to_h
p [["a", 1], ["b", 2]].to_h
p [["a", "x"], ["b", "y"]].to_h
p [[:x, 10], [:y, 20]].to_h

# A non-literal receiver (routed through a method parameter) cannot have its
# pair element types resolved statically, so the keys/values stay boxed and
# still render correctly.
def to_h_of(pairs)
  pairs.to_h
end
p to_h_of([["k", 1], ["m", 2]])
p to_h_of([[1, 10], [2, 20]])
