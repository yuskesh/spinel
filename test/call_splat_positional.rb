# Call-site positional splat `f(*args)`: a single trailing splat expands an
# array across a method's fixed parameters. Previously rejected at compile time
# (the callee was left untyped and dead-code-eliminated, or the splat arg failed
# to emit). The splat array's element type now propagates to the callee's params
# so it is emitted, the operand is evaluated once into a rooted temp, and a
# fixed-arity mismatch raises ArgumentError exactly as CRuby does.
def s(x); x; end

def add3(a, b, c); a + b + c; end
def join2(a, b); a + b; end

# splat of a local, a literal, and a method-call result
nums = [1, 2, 3]
p add3(*nums)
p add3(*[4, 5, 6])
p add3(*s([7, 8, 9]))

# string and float element kinds
p join2(*["x", "y"])
p join2(*[1.5, 2.5])

# splat after a fixed leading arg
rest = [20, 30]
p add3(10, *rest)

# value position vs statement position
total = add3(*nums)
p total
add3(*nums)  # statement: no output, must still compile

# optional params: a short splat falls back to the defaults
def opt(a, b = 9, c = 100); a + b + c; end
p opt(*[1])
p opt(*[1, 2])
p opt(*[1, 2, 3])
p opt(*s([5]))

# rest-param callee absorbs any length
def collect(*xs); xs.sum; end
p collect(*[1, 2, 3, 4])

# arity mismatch raises ArgumentError with CRuby's exact message
begin
  add3(*[1, 2])
rescue ArgumentError => e
  p e.message
end
begin
  join2(*[1, 2, 3])
rescue ArgumentError => e
  p e.message
end
begin
  opt(*[1, 2, 3, 4])
rescue ArgumentError => e
  p e.message
end
