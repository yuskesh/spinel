# <=> between incomparable operands returns nil (it raised NoMethodError);
# comparable pairs are unchanged, and a poly operand decides at runtime.
p(1 <=> "a")
p("a" <=> 1)
p(1.0 <=> "x")
p(:s <=> 3)
p(1 <=> 2)
p(1 <=> 2.0)
p("a" <=> "b")
p([1, 2] <=> [1, 3])
mixed = [1, "a", 2]
p(mixed[0] <=> mixed[1])
p(mixed[0] <=> mixed[2])
