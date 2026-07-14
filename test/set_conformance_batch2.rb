# Set mutators (flatten!/replace/reset), combinators with Array operands,
# and mixed Set/Array arguments reaching the same combinator call site.
require 'set'

# flatten! merges nested sets in place; returns nil when already flat
s = Set[Set[1, 2], Set[3]]
s.flatten!
p s.to_a.sort
p Set[1, 2].flatten!

# replace swaps the contents; reset returns self
s2 = Set[1, 2, 3]
p s2.replace([4, 5]).to_a.sort
p s2.reset.to_a.sort

# - / difference with an Array operand
p((Set[1, 2, 3] - [2]).to_a.sort)
p(Set[1, 2, 3].difference([2, 3]).to_a.sort)
p((Set[1, 2, 3] - Set[2]).to_a.sort)

# the same combinator fed both an Array and a Set in one program
p((Set[1, 2] | [3, 4]).to_a.sort)
p((Set[1, 2] | Set[2, 3]).to_a.sort)
p((Set[1, 2] & [2, 3]).to_a.sort)
p((Set[1, 2] & Set[2]).to_a.sort)
p((Set[1, 2] ^ [2, 3]).to_a.sort)
p((Set[1, 2] ^ Set[2, 3]).to_a.sort)
p((Set[1, 2] + [3]).to_a.sort)
p(Set[1, 2].union(Set[5]).to_a.sort)
