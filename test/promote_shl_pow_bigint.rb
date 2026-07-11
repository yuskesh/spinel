# Under --int-overflow=promote a runtime << past the word and a runtime **
# whose result overflows promote to Bignum (CRuby semantics); in-range
# results stay exact fixnums (the old ** went through double and lost
# precision above 2^53). Bignum receivers shift and power as Bignums.
a = 1
p(a << 70)
b = 2; e = 100
p(b ** e)
x = 3; p(x ** 5)
y = 1; p(y << 10)
z = 3; p(z ** 39)
big = 10 ** 30
p(big << 4)
p(big ** 2)
f = 1; (1..25).each { |k| f = f * k }; p(f)
