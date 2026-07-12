a = Complex(2, 3)
p a.frozen?
a.freeze
p a.frozen?
r = Rational(1, 2)
p r.frozen?
g = (1..3)
p g.frozen?
p 5.frozen?
p :sym.frozen?
p 1.5.frozen?
p nil.frozen?
p true.frozen?
