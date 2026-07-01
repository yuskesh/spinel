# The bundled `set` library: construction from an array (with de-duplication of
# equal scalar elements), iteration, membership, and mutation.
require 'set'

s = Set.new([:a, :b, :a, :c, :b])
p s.size            # 3 (duplicates dropped)
p s.length          # 3
p s.empty?          # false
p s.include?(:b)    # true
p s.member?(:z)     # false

s.each { |x| print x, " " }
puts

s << :d
s.add(:a)           # already present, no-op
p s.size            # 4
p s.include?(:d)    # true

p Set.new.empty?    # true (no argument)

# integers dedup the same way
n = Set.new([1, 2, 2, 3, 1])
total = 0
n.each { |x| total += x }
p total             # 6
p n.size            # 3
