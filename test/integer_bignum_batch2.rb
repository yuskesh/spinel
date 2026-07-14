# Integer/Bignum conformance (KieranP #2418-#2423, #2425)
p((2 ** 100).magnitude)
p((2 ** 100).abs)
p(1.coerce(2 ** 100))
p(Integer.sqrt(2 ** 100))
r = (5 & 2.0 rescue $!.class)
p r
p(1 | (2 ** 100))
p(1 ^ (2 ** 100))
a = 20
b = -2
p(a << b)
sh = 3
p(a << sh)
p(a >> sh)
p(7.div(2.5))
p(7.ceildiv(2.5))
p(6.abs2)
