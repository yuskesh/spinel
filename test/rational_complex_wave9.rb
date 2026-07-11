# Rational floor/ceil (0/1 arg), predicates, coerce, Rational ** Rational,
# Complex(String), ** float/Complex, to_i/to_f/to_r, numerator/denominator,
# fdiv, coerce. Float-exponent pow is approx-checked (libm last-digit).
p(Rational(7,2).floor)
p(Rational(7,2).ceil)
p(Rational(-7,2).floor)
p(Rational(-7,2).ceil)
p(Rational(11,4).round)
p(Rational(314,100).floor(1))
p(Rational(314,100).ceil(1))
p(Rational(0,1).zero?)
p(Rational(1,2).positive?)
p(Rational(-1,2).negative?)
p(Rational(1,2).coerce(2))
p(Rational(2,3) ** Rational(1,2))
p(Complex("2+3i"))
z = Complex(2,3) ** 2.0
puts (z.real.to_f + 5.0).abs < 1e-9 && (z.imaginary.to_f - 12.0).abs < 1e-9 ? "pow_float ok" : "pow_float bad"
p(Complex(2,3) ** Complex(1,0))
p(Complex(1,1).arg)
p(Complex(1,1).angle)
p(Complex(2,3).polar)
p(Complex(2,3).rect)
p(Complex(2,3).rectangular)
p(Complex.rect(2,3))
p(Complex.rectangular(2,3))
p(Complex(2,0).to_i)
p(Complex(2,3).numerator)
p(Complex(2,3).denominator)
p(Complex(2,3).fdiv(2))
p(Complex(2,3).coerce(2))
p(Complex(2,0).to_f)
p(Complex(2.5,0).to_r)
r = (begin; Complex(2,3).to_i; rescue RangeError => e; "RangeError"; end); p r
