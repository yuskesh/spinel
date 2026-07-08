# Anonymous `**` forwards keyword arguments the same way a named `**kw` does:
# `def m(**) = target(**)` threads the caller's kwargs through to target.
def target(a:, b:, c:) = [a, b, c]
def fwd(**) = target(**)
p fwd(a: 1, b: 2, c: 3)

# forwarding into a keyword-rest callee collects the kwargs into a hash
def collect(**opts) = opts
def fwd2(**) = collect(**)
p fwd2(x: 10, y: 20)

# parity with the named `**o` form
def fwd_named(**o) = target(**o)
p fwd_named(a: 4, b: 5, c: 6)

# an explicit keyword alongside the anonymous splat (literal + forward merge)
def prepend(**) = collect(z: 99, **)
p prepend(a: 1, b: 2)

# a positional param ahead of the anonymous `**` must not be disturbed
def with_pos(n, **) = [n, collect(**)]
p with_pos(7, k: 1)
