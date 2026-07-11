# Variable-receiver NilClass conversions captured into locals, variable
# Hash.new(default) folds, and the Complex query-method family.
n = nil; v = (n.to_a); p v
w = (n.to_r); p w
h = (n.to_h); p h
z = (n.to_c); p z
a = Hash.new(7); p(a.default)
b = Hash.new("d"); p(b["missing"])
p(Complex(0, 0).zero?)
p(Complex(2, 3).zero?)
p(Complex(2, 3).real?)
p(Complex(2, 3).integer?)
p(Complex(2, 3).finite?)
p(Complex(2, 3).infinite?)
p(Complex(1.0/0.0, 0).infinite?)
p(Complex(2, 3).eql?(Complex(2, 3)))
p(Complex(2, 3).eql?(Complex(2, 4)))
p(Complex(2, 0).rationalize)
