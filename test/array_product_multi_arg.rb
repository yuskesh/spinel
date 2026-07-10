# Array#product with two or more array arguments builds the n-way Cartesian
# product (one element from each input, in order). Previously only the single
# argument form had a codegen arm; 2+ arguments were rejected.
p [1, 2].product([3, 4], [5, 6])          # 8 triples
p [1, 2].product([3, 4])                   # single-arg form still works
p [1, 2, 3].product([4, 5], [6])           # mixed lengths
p [1].product([2], [3], [4])               # four inputs -> one 4-tuple
p [1, 2].product([3, 4], [])               # an empty input -> empty product

# Non-literal receiver and arguments (defeats constant folding).
def prod(a, b, c); a.product(b, c); end
p prod([1, 2], [3, 4], [5, 6])

# String and mixed element types.
p ["a", "b"].product([1, 2], [:x])         # tuples carry each element's type
