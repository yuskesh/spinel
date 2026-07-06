# Nested multiple-assignment targets bind through the nesting.
def s(x); x; end
(a, (b, c)), d = s([[1, [2, 3]], 4])
p [a, b, c, d]
(e, (f, g, h)), i = s([[10, [20, 30, 40]], 50])
p [e, f, g, h, i]
