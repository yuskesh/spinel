# Multi-assign with a TY_POLY right-hand side: the boxed value can hold an
# array at runtime, so it must be destructured element-by-element. The
# scalar fast path used to hand the whole array to the first target and
# nil-fill the rest.

def widen(x); x; end

pair = widen(["TROOA1", true])
name, mirrored = pair
puts name
puts mirrored
# TROOA1
# true

# through a poly hash read (the doom sprite-index shape)
index = widen({ "k" => ["POSSB2", false] })
lump, mir = index["k"]
puts lump
puts mir.inspect
# POSSB2
# false

# more targets than elements: trailing targets nil-fill
a, b, c = widen([1, 2])
puts a
puts b
puts c.nil?
# 1
# 2
# true

# a poly SCALAR RHS keeps Ruby semantics: first target takes the whole
# value, the rest nil-fill (no Integer#[] bit reads / String#[] chars)
x = widen(42)
a2, b2 = x
puts a2
puts b2.nil?
s2 = widen("hi")
c2, d2 = s2
puts c2
puts d2.nil?
h2 = widen({ k: 1 })
e2, f2 = h2
puts e2 == h2
puts f2.nil?
