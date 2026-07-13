# String conformance: to_f underscores, -@ frozen, clear as a value,
# byteslice OOB->nil / boundary / Range, === with a non-String arg.
p "1_000.5".to_f
p "3.14".to_f
p "abc".to_f
p "x".-@.frozen?
s = "abc"; v = s.clear; p v; p s
p "abc".byteslice(5)
p "abc".byteslice(3)
p "abc".byteslice(2)
p "abc".byteslice(3, 0)
p "hello".byteslice(1..3)
p "hello".byteslice(2..)
p "hello".byteslice(10..12)
p("abc" === nil)
p("abc" === "abc")
p("abc" === "xyz")
