# A callable *value* forwarded with `&` to an iterator -- `recv.<iter>(&f)` where
# `f` is a Proc value, a Method object, or the method's own `&block` param -- is
# desugared to `recv.<iter> { |x| f.call(x) }`, so it lowers through the normal
# block emitters for ANY iterator and element type, not a per-iterator special
# case. Receivers are method params so the receiver type is only known at runtime
# (no constant-folding of the literal away).
def ints(a, sq, odd, big)
  p a.map(&sq)        # Proc value -> int map
  p a.select(&odd)    # predicate select
  p a.reject(&odd)    # predicate reject
  total = 0
  acc = ->(x) { total += x }
  a.each(&acc)        # statement-form each (side effect)
  p total
  p a.map(&big)       # Proc passed in as a method parameter
end

# A broad iterator surface, all through the one desugar: aggregates and
# scalar-returning iterators, not just map/select.
def aggregates(a, key, pred)
  p a.sort_by(&key)
  p a.min_by(&key)
  p a.max_by(&key)
  p a.partition(&pred)
  p a.find(&pred)
  p a.count(&pred)
end

# String elements (a limitation the older value-callable path could not handle):
# the result type is known because the proc is a local lambda, so its return
# type is inferred. A proc passed *as a parameter* carries no inferred return
# type, so a non-int result there is out of scope (it mistypes for literal
# blocks too -- a separate proc-param return-inference gap).
def strings(words)
  up = ->(s) { s.upcase }
  p words.map(&up)
end

def ranges(sq)
  p (1..4).map(&sq)   # range receiver, not an array
end

# An inline `&->(x){...}` lambda is equivalent to the block itself; it forwards
# like any callable (the int element cases; a non-int result shares the
# proc-param return-inference gap noted above).
def inline_lambdas(a)
  p a.map(&->(x) { x * 3 })
  p a.find(&->(x) { x > 2 })
  p a.count(&->(x) { x.odd? })
end

# Method objects: `&m` (a local Method value) and `&method(:m)` (inline).
def dbl(x) = x * 2
def positive?(x) = x > 0
def methods_forward(a)
  m = method(:dbl)
  p a.map(&m)
  p a.select(&method(:positive?))
  p a.map(&method(:dbl))
end

# Forwarding the method's own `&block` parameter to an iterator -- the canonical
# "pass the block along" pattern (previously a hard "unsupported call").
def relay_map(a, &b)
  a.map(&b)
end
def relay_each(a, &b)
  a.each(&b)
end
def relay_select(a, &b)
  a.select(&b)
end

square = ->(x) { x * x }
ints([1, 2, 3, 4], square, ->(x) { x.odd? }, ->(x) { x + 100 })
aggregates([3, 1, 4, 2], ->(x) { -x }, ->(x) { x.even? })
strings(["ab", "cd"])
ranges(square)
inline_lambdas([1, 2, 3, 4])
methods_forward([-1, 2, -3, 4])
p relay_map([1, 2, 3]) { |x| x + 1 }
collected = []
relay_each([1, 2, 3]) { |x| collected << x * 10 }
p collected
p relay_select([1, 2, 3, 4]) { |x| x.even? }
