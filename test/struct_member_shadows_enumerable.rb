# A Struct/Data member accessor always beats the inherited Enumerable
# surface: CRuby defines accessors directly on the subclass, so a member
# named `count` (or `min`, `first`, ...) shadows Enumerable's method of
# the same name. The synthesized-#each Enumerable lowering wrapped every
# non-chain method into __enum_to_a without checking the member table, so
# `Data.define(:count).new(count: 7).count` returned the MEMBER TOTAL (1)
# instead of the member value (7) - and a Data model folded through #with
# froze at its first value.

Model = Data.define(:count)
m = Model.new(count: 7)
puts m.count
m2 = m.with(count: m.count + 1)
puts m2.count

Pair = Data.define(:count, :min)
pr = Pair.new(count: 40, min: 9)
puts pr.count
puts pr.min

# Struct spelling: members named after Enumerable terminals still read as
# members...
SFirst = Struct.new(:first, :count)
s = SFirst.new(10, 20)
puts s.first
puts s.count

# ...while Enumerable methods NOT named after a member still ride the
# synthesized each (the wave's feature stays intact; Struct, unlike Data,
# includes Enumerable in CRuby).
Wide = Struct.new(:a, :b, :c)
w = Wide.new(3, 1, 2)
puts w.sum
puts w.include?(2)
puts w.map { |v| v * 10 }.join(",")
puts w.min
