# Enumerable#to_set through method parameters: the receiver reaches
# Set.new intact (including via chained-call receivers), the result
# dedups, prints, and supports set algebra.
require "set"

def ts_int(a) = a.to_set
s1 = ts_int([1, 2, 2, 3])
p s1.class
p s1.size
p s1.include?(2)
p s1

def ts_str(a) = a.to_set
p ts_str(%w[a b a]).size

def ts_poly(a) = a.to_set
p ts_poly([1, "one", :one, 1]).size

p (1..4).to_set.size

def union(a, b) = a.to_set | b.to_set
p union([1, 2], [2, 3]).sort

direct = Set.new([5, 6, 5])
p direct.size
p Set.new.empty?
