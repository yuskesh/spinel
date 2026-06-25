# Enumerator.new with the bare `y.yield v` form (no parentheses). Prism parses
# `y.yield v` and `y.yield(v)` into the same CallNode (the parens only change
# opening_loc), so the yielder rewrite that turns `recv.yield ...` into
# sp_Fiber_yield handles both forms identically. Covers single/multi/zero args,
# a variable argument, the last-statement position, and an infinite generator
# driven by `loop`.

e = Enumerator.new do |y|
  a = 10
  y.yield a          # variable arg
  b = 20
  y.yield a, b       # multiple args -> array
  y.yield             # no args -> nil
  y.yield a + b       # last statement
end
p e.next
p e.next
p e.next
p e.next

# Infinite generator with bare yield inside loop, consumed by take / next.
fib = Enumerator.new do |y|
  a = 0
  b = 1
  loop do
    y.yield a
    a, b = b, a + b
  end
end
p fib.take(6)
p fib.next
p fib.next

# StopIteration after a finite bare-yield generator.
g = Enumerator.new do |y|
  y.yield 1
  y.yield 2
end
p g.peek
p g.next
p g.next
begin
  g.next
rescue StopIteration
  puts "stopped"
end
