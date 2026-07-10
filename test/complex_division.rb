# Complex#/ by a real scalar divides each component. Boxing the real into c+0i
# and running the conjugate formula produced NaN at a zero divisor; MRI divides
# componentwise, so a Float zero yields Infinity and an Integer zero raises
# ZeroDivisionError (integer division rules).
def df(a, b); a / b; end
p df(Complex(20, 40), 0.0)              # (Infinity+Infinity*i)
p df(Complex(21, 41), 2.0)             # (10.5+20.5i)
p df(Complex(-20, 40), 0.0)            # (-Infinity+Infinity*i)

def dnan(a, b); a / b; end
p dnan(Complex(0, 0), 0.0)             # (NaN+NaN*i)

def di(a, b); a / b; end
p di(Complex(20, 40), 4)               # (5+10i)

def dz(a, b); a / b; end
begin
  dz(Complex(20, 40), 0)
  puts "no raise"
rescue ZeroDivisionError => e
  puts e.message                        # divided by 0
end

# Complex / Complex still uses the full division.
def dc(a, b); a / b; end
p dc(Complex(20, 40), Complex(2, 0))   # (10+20i)

# Infinity / NaN component formatting in inspect and to_s.
def inf(a, b); a / b; end
p inf(Complex(3, 0), 0.0).to_s          # "Infinity+NaN*i"  (0/0.0 imag is NaN)
