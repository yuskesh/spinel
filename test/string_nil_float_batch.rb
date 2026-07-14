# String#bytesplice with Range / #append_as_bytes / #crypt,
# Float#=== against non-float operands, and nil & / | / ^ boolean ops
# on a receiver that widened to poly.

# bytesplice: integer start/length and Range forms
s = "hello"
s.bytesplice(0, 2, "HE")
p s
s = "hello"
s.bytesplice(1..3, "___")
p s

# append_as_bytes
s = "abc"
s.append_as_bytes("de")
p s

# crypt(3): real libc DES with a two-character salt
p "hello".crypt("ab")
p "password".crypt("sa")

# Float#=== : nil / Rational / Complex / Integer / Float operands
p(1.0 === nil)
p(1.0 === Rational(1, 2))
p(1.0 === Complex(1, 0))
p(0.5 === Rational(1, 2))
p(1.0 === 1)
p(1.0 === 1.0)
p(1.0 == nil)
p(1.0 == Rational(1, 2))

# nil & / | / ^ stay boolean even when the local widens to poly
n = nil
p(n | 0)
p(n & 0)
p(n ^ 0)
n = nil
p(n | "x")
p(n & "x")
p(n ^ false)
