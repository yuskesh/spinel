# Array#permutation on integer arrays, mirroring combination: blockless returns
# an array of the k-element permutations; the block form yields each one.
p [1, 2, 3].permutation(2).to_a
p [1, 2, 3].permutation.to_a          # argless = full-length permutations
p [1, 2, 3].permutation(0).to_a       # one empty permutation
p [1, 2].permutation(5).to_a          # k > length -> none
p [1, 2, 3].permutation(2).map { |pr| pr.sum }
acc = []
[1, 2, 3].permutation(2) { |pr| acc << pr.sum }
p acc
