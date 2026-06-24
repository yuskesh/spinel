# A proc/lambda whose parameter is polymorphic (called with values of
# differing type) and whose body returns that parameter must hand back the
# value unchanged, not collapse it to 0. The value rides the proc-call poly
# side-channel, which the call site must populate even for a concrete argument.
f = proc { |x| x }
p f.call(42)
p f.call("hi")
p f.call(:sym)
p f.call(3.5)

g = ->(x) { x }
p g.call(7)
p g.call("yo")

double = lambda { |n| n }
p double.call(100)
p double.call("z")

# Multi-argument proc: every argument (not just the first) must reach the callee
# through the boxed side-channel; exercises the per-arg temp + root path.
pair = proc { |a, b| "#{a}=#{b}" }
p pair.call("x", "y")
p pair.call(1, 2)

# A proc returning a poly array built from its (poly) params.
mk = proc { |a, b| [a, b] }
r = mk.call(1, "two")
p r

# Heap args evaluated in sequence (a later allocating arg must not collect the
# earlier one): each call boxes fresh strings into adjacent side-channel slots,
# and the proc consumes all three. Returns an int so the check stays on the
# argument path (the call's value is the concatenated length).
widths = proc { |a, b, c| (a + b + c).length }
10.times { |i| p widths.call("a#{i}", "bb#{i}", "ccc#{i}") }
