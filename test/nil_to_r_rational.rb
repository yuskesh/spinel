# NilClass#to_r returns Rational(0, 1), not float zero.
p nil.to_r                       # (0/1)
p nil.to_r == Rational(0, 1)     # true
p nil.to_r + Rational(1, 2)      # (1/2)
