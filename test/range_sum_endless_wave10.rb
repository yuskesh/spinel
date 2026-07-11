# Range#sum with a numeric init, endless-literal size (Infinity) and
# take/first(n) (counted prefix), and beginless size raising TypeError.
p((1..3).sum(10))
p((1..4).sum(0.5))
p((1..).size)
r6 = (begin; (..5).size; rescue TypeError; "TE"; end); p r6
p((1..).take(3))
p((1..).first(2))
p((5..).take(4))
