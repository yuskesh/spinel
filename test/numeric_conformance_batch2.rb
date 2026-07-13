# Bignum -@ / round / ceil / floor (incl. negative precision); Float <=> NaN
# is nil; Float arg/angle/phase (Integer 0 or Float PI by sign).
b = 2 ** 100
p(-b)
p(-(-b))
p b.round
p b.round(-2)
p b.floor(-2)
p b.ceil(-2)
p((0 - b).round(-2))
p(Float::NAN <=> 1.0)
p(1.0 <=> 2.0)
p(2.0 <=> 2.0)
p(Float::NAN <=> Float::NAN)
p((-1.5).angle)
p(1.5.arg)
p(1.5.phase)
