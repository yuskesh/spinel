# Proc composition through poly-param procs, curry saturation, Proc.new(&b)
# forwarding a captured block, and explicit Method#to_proc.

# forward and backward composition
inc = ->(a) { a + 1 }
dbl = ->(a) { a * 2 }
p (inc >> dbl).call(5)
p (inc << dbl).call(5)

# composition through a method boundary (type-erased operands)
def chain(x); (->(a) { a + 1 } >> ->(a) { a * 2 }).call(x); end
p chain(5)

# curry saturates through a method param
def apply2(pr); pr.curry.call(1).call(2); end
p apply2(->(a, b) { a + b })

# curry on a local proc, single-call and bracket application
add3 = ->(a, b, c) { a + b + c }
p add3.curry[1][2][3]
cur = add3.curry.call(10)
p cur.call(20).call(30)

# Proc.new(&b) returns the forwarded block as a proc
def make(&b); Proc.new(&b); end
pr = make { |x| x * 2 }
p pr.call(5)
p pr.call(7)

# proc(&b) behaves the same
def wrap(&b); proc(&b); end
w = wrap { |x| x + 100 }
p w.call(1)

# explicit Method#to_proc
def triple(x); x * 3; end
tp = method(:triple).to_proc
p tp.call(5)
p [1, 2, 3].map(&method(:triple))
