# inject/reduce with the operator Symbol held in a VARIABLE: the sole-
# assignment lookthrough resolves the op statically, in both the emit and
# the result-type inference (was: typed as the symbol itself).
s = :+
p [3, 1, 2].inject(s)
p [3, 1, 2].reduce(s)
m = :*
p [2, 3, 4].reduce(m)
p [2.5, 1.5].inject(10.0, s)
