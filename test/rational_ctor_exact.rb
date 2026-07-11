# Kernel#Rational with Float and Rational operands keeps the exact value
# (5/2 for 2.5) instead of truncating through an int cast. Per-operand-kind
# helpers: a mixed-type param would widen to poly, which has no ctor arm.
def f1f(a) = Rational(a)
def f1r(a) = Rational(a)
def f2f(a, b) = Rational(a, b)
def f2i(a, b) = Rational(a, b)

p f1f(2.5)                            # (5/2)
p f1f(-0.75)                          # (-3/4)
p f1f(3.0)                            # (3/1)
p f1r(Rational(7, 4))                 # (7/4)
p f2f(2.5, 0.5)                       # (5/1)
p f2f(1.0, 3.0)                       # (1/3)
p f2i(1, 3)                           # (1/3)
p Rational(Rational(1, 2), Rational(1, 3))  # (3/2)
p Rational(1.5, 3)                    # (1/2)
begin
  f2f(1.0, 0.0)
rescue ZeroDivisionError => e
  puts "#{e.class}: #{e.message}"
end
