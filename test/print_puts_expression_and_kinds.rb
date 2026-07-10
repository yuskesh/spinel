# print renders Array/Hash/objects via to_s (it compile-errored on an Array
# and printed blank for objects); puts/print compose as expressions
# returning nil, like p.
print [1, 2, 3]
print "\n"
print({ a: 1 })
print "\n"
print 1..3
print "\n"
x = puts("hi")
p x
r = print("a")
puts
p r
puts (puts 7)
