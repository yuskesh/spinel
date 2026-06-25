# Enumerator.new { |y| ... } is a fiber-backed external generator: `y << v` and
# `y.yield(v)` produce values lazily, #next / #peek walk them, the generator
# terminates with StopIteration, and #rewind restarts it. Works with infinite
# generators via #take / #first. (`y.yield v` without parentheses is not
# supported; use `y << v` or `y.yield(v)`.)

e = Enumerator.new do |y|
  y << 1
  y.yield(2)
  y << 3
end
p e.class
p e.next
p e.peek        # does not advance
p e.next
p e.next
begin
  e.next
rescue StopIteration
  puts "stopped"
end
e.rewind
p e.next        # back to the start

# An infinite generator + take / first (a fresh run each time, independent of
# the #next cursor).
fib = Enumerator.new do |y|
  a, b = 0, 1
  loop { y << a; a, b = b, a + b }
end
p fib.take(8)
p fib.first(5)
p fib.next
p fib.next

# A generator that captures an outer (read-only) value.
base = 100
g = Enumerator.new do |y|
  y << base
  y << base + 1
  y << base + 2
end
p g.take(3)
p g.next
p g.next

# is_a? / class
p e.is_a?(Enumerator)
