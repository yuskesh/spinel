# A Fiber misused in ways CRuby rejects must raise FiberError, not crash:
# yielding from the root fiber, yielding inside a fiber entered via #transfer,
# and resuming a fiber that is still running (double resume). Each is catchable.

# Yielding outside any fiber (from the root fiber) raises rather than crashing.
begin
  Fiber.yield(1)
  puts "no raise (root yield)"
rescue FiberError => e
  puts "raised (root yield): #{e.class}: #{e.message}"
end

# A fiber entered with #transfer has no resumer to yield back to.
begin
  ft = Fiber.new { |x| Fiber.yield x }
  ft.transfer(1)
  puts "no raise (transfer yield)"
rescue FiberError => e
  puts "raised (transfer yield): #{e.class}: #{e.message}"
end

# Resuming a fiber that is still running (an ancestor on the resume stack) is a
# double resume. Both fibers are defined at top level and reached via globals so
# the bodies capture no locals.
$f1 = nil
$f2 = nil
$f1 = Fiber.new { $f2.resume }
$f2 = Fiber.new { $f1.resume }   # $f1 is still running when this runs
begin
  $f1.resume
  puts "no raise (double resume)"
rescue FiberError => e
  puts "raised (double resume): #{e.class}: #{e.message}"
end

# The guards do not disturb a well-behaved fiber: resume/yield round-trips,
# the terminal value comes back, and #alive? flips to false at the end.
g = Fiber.new { |a| b = Fiber.yield(a + 1); Fiber.yield(b * 2); 99 }
p g.resume(10)
p g.resume(5)
p g.resume(0)
p g.alive?

# An infinite generator still works.
fib = Fiber.new do
  a, b = 0, 1
  loop { Fiber.yield a; a, b = b, a + b }
end
p Array.new(8) { fib.resume }
