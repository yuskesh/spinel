# Float#numerator/#denominator (exact rational of the double), Math.frexp,
# and Math.sqrt coercing a Rational argument.
def num(a) = a.numerator
def den(a) = a.denominator
def fx(x) = Math.frexp(x)
def sq(r) = Math.sqrt(r)

p num(0.5)          # 1
p den(0.5)          # 2
p num(-0.75)        # -3
p den(-0.75)        # 4
p num(3.0)          # 3
p den(3.0)          # 1
p fx(8.0)           # [0.5, 4]
p fx(0.5)           # [0.5, 0]
p fx(-12.0)         # [-0.75, 4]
p Math.frexp(1)     # [0.5, 1]
p sq(Rational(1, 4))     # 0.5
p sq(Rational(9, 1))     # 3.0
p Math.sqrt(Rational(1, 16))  # 0.25
begin
  Math.sqrt("x")
rescue TypeError => e
  puts "#{e.class}: #{e.message}"
end
