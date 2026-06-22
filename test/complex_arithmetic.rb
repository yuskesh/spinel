# Complex arithmetic over float components (re/im are mrb_float; CRuby keeps
# Integer/Rational components, so .real and exact division differ -- see
# limitations). Operators and display for the common cases match CRuby.
a = Complex(1, 2)
b = Complex(3, -1)
puts a
puts a.inspect
p a
p Complex(2, 3)
puts(a + b)
puts(a - b)
puts(a * b)
puts(a ** 2)
puts(a ** 3)
puts(-a)
puts(+a)
puts a.conjugate
puts a.conj
puts a.abs
puts(Complex(3, 4).abs)
puts(a == Complex(1, 2))
puts(a == b)
puts(a != b)
puts "c=#{a} sum=#{a + b}"
puts(2 + Complex(0, 3))
puts(1 + 2i)
puts(5 - Complex(1, 1))
puts(Complex(2, 0) == 2)
puts(Complex(8, 4) / Complex(2, 0))
puts(Complex(6, 2) / 2)
def cadd(x, y); x + y; end
puts(cadd(Complex(1, 1), Complex(2, 2)))
