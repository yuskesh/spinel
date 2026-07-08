# A proc/lambda passed as a positional argument escapes into an opaque method
# parameter and is called through the generic type-erased proc ABI. Its own
# argument types are not statically knowable, so a Float argument must survive
# (an int-typed slot would truncate it to 0).
def call1(pr, x) = pr.call(x)
def call2(pr, a, b) = pr.call(a, b)

puts call1(->(a) { a * 2 }, 0.25)
puts call1(->(a) { a * 2 }, 3)
puts call2(->(a, b) { a + b }, 0.5, 0.25)
puts call1(->(s) { s.upcase }, "hi")
puts call1(proc { |a| a * 10 }, 0.1)

r = call1(->(a) { a * 2 }, 1.5)
puts(r + 1)
