# A splat in the middle of a call keeps the trailing positional args, and
# forwarding *args/**kw does not drop the keyword arguments.
def g(a, b, c, d) = [a, b, c, d]
def mid(m) = g(1, *m, 4)
p mid([2, 3])
def g5(a, b, c, d, e) = [a, b, c, d, e]
def mid5(m) = g5(1, *m, 5)
p mid5([2, 3, 4])
def gk(a, b, c: 0) = [a, b, c]
def fwd(*args, **kw) = gk(*args, **kw)
p fwd(1, 2, c: 3)
p fwd(1, 2)
