# Rational arithmetic over mrb_int num/den (Phase 1; overflow raises).
a = Rational(2, 4)
b = Rational(1, 3)
puts a
puts a.inspect
puts(a + b)
puts(a - b)
puts(a * b)
puts(a / b)
puts(a ** 3)
puts(a ** -2)
puts a.numerator
puts a.denominator
puts a.to_f
puts a.to_i
puts(-a)
puts a.abs
puts Rational(-3, 9).abs
puts "interp: #{a} and #{a + b}"
p a
p Rational(7, 2)

# comparisons
puts(a < b)
puts(a > b)
puts(a <= Rational(1, 2))
puts(a == Rational(1, 2))
puts(a <=> b)

# mixed Integer/Rational
puts(1 + Rational(1, 2))
puts(2 / 3r)
puts(3 * Rational(2, 9))
puts(Rational(1, 2) + 1)
puts(Rational(3, 1) == 3)
puts(1 < Rational(3, 2))

# through a method (param typing)
def sumr(x, y)
  x + y
end
puts(sumr(Rational(1, 4), Rational(1, 4)))

# Integer#quo
puts(10.quo(4))
