# Float <op> Rational (either side) coerces to Float like CRuby;
# comparisons are bool; ** goes through pow.
p(0.5 + Rational(1, 2))
p(0.5 - Rational(1, 2))
p(0.5 * Rational(1, 2))
p(0.5 / Rational(1, 2))
p(Rational(3, 2) + 0.25)
p(Rational(1, 2) == 0.5)
p(0.75 > Rational(1, 2))
p(2.0 ** Rational(1, 2))
