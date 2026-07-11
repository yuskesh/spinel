# Mixed Complex/Rational arithmetic computes in floats: the Rational operand
# coerces via #to_f (docs/limitations.md "Rational precision and Complex
# components"). The component CLASS deliberately differs from CRuby (float
# instead of rational), so this pin compares float-normalized components,
# which agree in both -- keeping the .expected oracle-regenerable.
def comps(c)
  [c.real.to_f, c.imaginary.to_f]
end
p comps(Complex(1, 2) + Rational(1, 2))
p comps(Complex(1, 2) - Rational(1, 2))
p comps(Complex(1, 2) * Rational(1, 2))
p comps(Complex(1, 2) / Rational(1, 2))
p comps(Rational(1, 2) + Complex(1, 2))
p comps(Rational(3, 2) * Complex(2, 4))
p comps(Complex(Rational(1, 2), Rational(1, 3)))
p comps(Complex(Rational(1, 2)))
p comps(Complex(3, 4).quo(2))
p comps(Complex(3, 4).quo(Rational(1, 2)))
p(10.quo(4))
