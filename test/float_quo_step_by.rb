# Float#quo is float division (Integer#quo stays Rational), and
# Numeric#step keyword forms (to:/by:, in either order, and the
# limit-plus-by mix) lower to the positional form.
p 10.0.quo(3)
p 2.5.quo(0.5)
1.0.step(2.0, by: 0.5) { |f| p f }
1.0.step(by: 0.25, to: 1.5) { |f| p f }
5.step(1, by: -2) { |i| p i }
