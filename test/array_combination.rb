# Array#combination(n) without a block materializes into the n-combinations via to_a.
def s(x); x; end
p s([1, 2, 3]).combination(2).to_a
p s([1, 2, 3]).combination(1).to_a
p s([1, 2, 3]).combination(3).to_a
p s([1, 2, 3]).combination(0).to_a
p s([1, 2, 3]).combination(4).to_a
