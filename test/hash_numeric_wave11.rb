# Hash#any?/none?/one?/count with a pair argument; blockless none? on {} local
p({ a: 1 }.any?([:a, 1]))
p({ a: 1 }.any?([:a, 2]))
b11 = {}; p(b11.none?)
c11 = {}; p(c11.any?)
h11 = { x: 5 }
p h11.count([:x, 5])
p h11.one?([:x, 5])
# hash literal as block arg
p([:a, :b].map(&{ a: 1, b: 2 }))
# Range#size: INFINITY bound, float begin TypeError, finite float end
p((1..Float::INFINITY).size)
r11 = ((1.0..10.0).size rescue "TypeError"); p r11
r12 = ((0.5..Float::INFINITY).size rescue "TypeError"); p r12
p((1..2.5).size)
p((1...2.5).size)
p((1...3.0).size)
# Integer modulo/remainder/divmod with Float and Rational divisors
p(7.remainder(2.5))
p(-7.remainder(2.5))
p(7.modulo(2.5))
p(10.divmod(3r))
p(10 % Rational(3, 1))
p(10.modulo(Rational(3, 1)))
p(1.5 % Rational(1, 2))
# Float#clamp: range keeps bound classes, mixed 2-arg boxed
p(5.5.clamp(1.0..3.0))
p(2.0.clamp(1.0..3.0))
p(0.5.clamp(1.0..3.0))
p(0.5.clamp(1, 3))
p(5.5.clamp(1, 3))
p(2.5.clamp(1, 3.5))
