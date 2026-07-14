# String conformance (KieranP #2369, #2370)
a = "abc".freeze
p((-a).equal?(a))              # #2369 frozen receiver: -@ is identity
s0 = "x" + "y"
f = -s0
p f.frozen?
p((-("a" + "b")).equal?("a" + "b"))
s = "hi"
p(s[0] = "Y")                  # #2370 []= consumed as a value
p s
t = "hi"
v = (t[0] = "Y")
p v
p t
u = "hi"
p(u[0, 2] = "AB")
p u
w = "abc"
p(w["bc"] = "XY")
p w
x = "abcd"
p(x[1..2] = "Z")
p x
p("abc".clear)                 # #2370 clear on a literal receiver
