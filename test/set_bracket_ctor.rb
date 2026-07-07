# Set[...] class-method constructor (CRuby's Set.[]).
require "set"

s = Set[1, 2, 3, 2, 1]
puts s.size
puts s.include?(2)
puts s.include?(9)

empty = Set[]
puts empty.empty?

t = Set["a", "b", "a"]
puts t.size
t << "c"
puts t.size
