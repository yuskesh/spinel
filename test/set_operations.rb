# Set operators and queries over the array-backed bundled set: & | - build
# fresh sets, subset?/superset? compare membership, to_a/map expose the
# elements (map returns an Array, as in CRuby).
require "set"
a = Set[1, 2, 3]
b = Set[2, 3, 4]
p (a & b).size
p (a | b).size
p (a - b).size
p a.subset?(b)
p Set[2, 3].subset?(a)
p a.superset?(Set[1, 2])
p Set[1, 2].to_a.sort
p Set[1, 2, 3].map { |x| x * 2 }.sort
p (a & b).to_a.sort
p (a | b).to_a.sort
p (a - b).to_a.sort
p a.intersection(b).size
p a.union(b).size
p a.difference(b).size
