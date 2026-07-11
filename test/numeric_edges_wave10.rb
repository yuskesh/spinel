# Integer bit-slice n[range], gcd/lcm TypeError on Float, divmod/modulo
# with Float divisors, clamp with Float-bounded ranges (both receivers),
# round(half:), and FloatDomainError on Infinity/NaN roundings.
p(5[1..3])
p(0b1101[0..2])
r = (begin; 5.gcd(2.0); rescue TypeError; "TypeError"; end); p r
r2 = (begin; 10.divmod(2.5); rescue; "raised"; end); p r2
p(10.divmod(3))
p(7.clamp(1.0..3.5))
p(2.5.clamp(1..3))
p(10.round(half: :even))
p(11.round(half: :up))
inf = Float::INFINITY
r3 = (begin; inf.floor; rescue FloatDomainError; "FDE"; end); p r3
r4 = (begin; (1.0/0.0).round; rescue FloatDomainError; "FDE2"; end); p r4
p(7.0.divmod(inf))
p(0.5.clamp(1..3))
p(7.0.divmod(-inf))
p(25.round(-1, half: :down))
p(15.round(-1, half: :even))
p(25.round(-1, half: :even))
p(25.round(-1, half: :up))
r5 = (begin; inf.round(-1); rescue FloatDomainError => e; e.message; end); p r5
p(inf.floor(1))
