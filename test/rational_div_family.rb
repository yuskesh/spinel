# Rational quo / fdiv / div, and round / truncate with a precision argument.
# Receivers and (for the poly cases) precisions flow through method params so
# the runtime dispatch path is exercised, not the constant folder.
# One helper per operand kind: a mixed-type param would widen to poly, and a
# poly operand intentionally falls through the Rational arithmetic arms.
def q_r(a, b) = a.quo(b)
def q_i(a, b) = a.quo(b)
def q_f(a, b) = a.quo(b)
def fd_r(a, b) = a.fdiv(b)
def fd_i(a, b) = a.fdiv(b)
def fd_f(a, b) = a.fdiv(b)
def dv_r(a, b) = a.div(b)
def dv_i(a, b) = a.div(b)
def dv_f(a, b) = a.div(b)
def rnd(a) = a.round
def rnd2(a) = a.round(2)
def rnd0(a) = a.round(0)
def rndm1(a) = a.round(-1)
def rndn(a, n) = a.round(n)
def trc2(a) = a.truncate(2)
def trcm1(a) = a.truncate(-1)
def trcn(a, n) = a.truncate(n)

r = Rational(22, 7)
# quo: Rational / Integer / Float operands
p q_r(Rational(1, 2), Rational(1, 3))   # (3/2)
p q_i(Rational(3, 4), 2)                # (3/8)
p q_f(Rational(3, 4), 0.5)              # 1.5
p q_r(Rational(-1, 2), Rational(1, 3))  # (-3/2)
# fdiv: always Float
p fd_r(Rational(3, 2), Rational(1, 2))  # 3.0
p fd_i(Rational(3, 2), 3)               # 0.5
p fd_f(Rational(3, 2), 0.5)             # 3.0
# div: floor division to Integer
p dv_r(Rational(7, 2), Rational(1, 1))  # 3
p dv_r(Rational(-7, 2), Rational(1, 1)) # -4
p dv_i(Rational(7, 2), 2)               # 1
p dv_i(Rational(-7, 2), 2)              # -2
p dv_f(Rational(7, 2), 1.5)             # 2
# round: ties away from zero; literal precisions pick the class statically
p rnd(r)                   # 3
p rnd(Rational(-22, 7))    # -3
p rnd(Rational(5, 2))      # 3
p rnd(Rational(-5, 2))     # -3
p rnd2(r)                  # (157/50)
p rnd0(r)                  # 3
p rndm1(Rational(157, 2))  # 80
# non-literal precision: class chosen from the runtime value
p rndn(r, 2)               # (157/50)
p rndn(r, 0)               # 3
p rndn(r, -1)              # 0
# truncate: toward zero
p trc2(r)                  # (157/50)
p trcm1(Rational(157, 2))  # 70
p trcn(r, 1)               # (31/10)
p trcn(r, 0)               # 3
# zero divisors raise
begin
  dv_r(Rational(1, 2), Rational(0, 1))
rescue ZeroDivisionError => e
  puts "#{e.class}: #{e.message}"
end
begin
  q_r(Rational(1, 2), Rational(0, 1))
rescue ZeroDivisionError => e
  puts "#{e.class}: #{e.message}"
end
