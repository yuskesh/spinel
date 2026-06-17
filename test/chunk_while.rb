# Array#chunk_while { |a, b| } .to_a groups adjacent elements into runs: a and b
# stay in the same run while the block is true, and a boundary falls where it is
# false. Materialized as a first-class array of runs (poly array of int arrays),
# so p / length / indexing work. Route the receiver through a method param.
def ia(x) = x

n = ia([1, 2, 4, 5, 7])
p n.chunk_while { |a, b| b - a == 1 }.to_a        # [[1, 2], [4, 5], [7]]
p n.chunk_while { |a, b| b - a == 1 }.to_a.length # 3

# all-together and all-split boundaries
p ia([1, 2, 3, 4]).chunk_while { |a, b| b - a == 1 }.to_a   # [[1, 2, 3, 4]]
p ia([1, 3, 5]).chunk_while { |a, b| b == a + 1 }.to_a      # [[1], [3], [5]]
p ia([42]).chunk_while { |a, b| true }.to_a                 # [[42]]

# a non-difference predicate, plus indexing into the runs
runs = ia([1, 2, 3, 1, 2]).chunk_while { |a, b| a <= b }.to_a
p runs        # [[1, 2, 3], [1, 2]]
p runs[0]     # [1, 2, 3]
p runs[1]     # [1, 2]

p ia([5, 5, 6, 7, 3]).chunk_while { |a, b| b >= a }.to_a    # [[5, 5, 6, 7], [3]]
