# Float#round/#truncate/#floor/#ceil with a runtime ndigits argument choose the
# class the way CRuby does: Integer when ndigits <= 0, Float when ndigits > 0.
def rnd(x, n); x.round(n); end
def trc(x, n); x.truncate(n); end
def flr(x, n); x.floor(n); end
p rnd(2.5, 0).class
p rnd(1234.5678, -2)
p rnd(3.14159, 2)
p rnd(3.14159, 2).class
p trc(3.14159, 0)
p trc(1234.5, -2)
p trc(9.87, 1)
p flr(37.4, -1)
# Extreme ndigits: the 10**n scale factor overflows to Inf; the result must stay
# CRuby-faithful (value unchanged for large n > 0, 0 for large n <= 0) not NaN.
p rnd(1.23, 400)
p rnd(1.23, -400)
p trc(2.5, 1000)
p flr(9.9, -500)
