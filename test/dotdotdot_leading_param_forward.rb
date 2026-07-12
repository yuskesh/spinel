# `def f(a, ...)` binds the leading param and forwards only the rest; plain
# `def f(...)` forwards everything.
def g(b, c) = [b, c]
def one_leading(a, ...) = g(...)
p one_leading(1, 2, 3)
def g3(x, y, z) = [x, y, z]
def two_leading(a, b, ...) = g3(...)
p two_leading(1, 2, 3, 4, 5)
def plain(...) = g(...)
p plain(9, 8)
def gk(x, k:) = [x, k]
def fwd_kw(a, ...) = gk(...)
p fwd_kw(1, 2, k: 3)
