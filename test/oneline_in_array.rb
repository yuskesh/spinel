# One-line `expr in pattern` with an array pattern yields a boolean.
def s(x); x; end
a = (s([1, 2, 3]) in [1, *]);    p a
b = (s([1, 2, 3]) in [1, 2, 3]); p b
c = (s([1, 2, 3]) in [1, 2]);    p c
