# Kernel#Array(x): nil becomes [], an array passes through (preserving its
# element type), and any other value is wrapped in a one-element array. The
# wrapped cases route through the runtime coercion; an argument statically
# known to be an array is returned directly.
def ints(a); a; end

p Array(nil)
p Array([1, 2, 3])
p Array(1)
p Array("x")
p Array(:sym)
p Array(3.5)
p Array(ints([4, 5, 6]))

# a poly argument is dispatched at run time (nil / array / scalar)
def coerce(x); Array(x); end
p coerce(nil)
p coerce(7)
p coerce([8, 9])
p coerce("z")

# an existing array is returned by identity, not copied: mutating the result of
# Array(arr) is visible through the original (a poly array reached at run time).
pa = [1, "a"]
r = coerce(pa)
r.push("z")
p pa.length

# a scalar argument yields a precisely typed array, so a typed-array operation
# applies directly to the result.
p Array(5).sum
p Array(2.5).first
p Array("hi").first.upcase
