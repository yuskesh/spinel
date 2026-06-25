# Fiber#kill terminates a fiber, running its ensure blocks. The kill signal
# unwinds through ensure clauses but is NOT caught by user rescue clauses, and
# the fiber does not resume past the kill point.

# Suspended fiber: ensure runs, the fiber is left dead, kill returns the fiber.
h = Fiber.new do
  begin
    Fiber.yield 1
    Fiber.yield 2
  ensure
    puts "h ensure ran"
  end
end
h.resume
p h.kill.is_a?(Fiber)   # kill returns the fiber
p h.alive?              # now terminated

# kill is idempotent: a dead fiber stays dead.
h.kill
p h.alive?

# A bare rescue does NOT swallow the kill; only the ensure runs, and execution
# does not continue past the kill point.
r = Fiber.new do
  begin
    Fiber.yield 1
    Fiber.yield 2
  rescue => e
    puts "SHOULD NOT RESCUE: #{e.class}"
  ensure
    puts "r ensure ran"
  end
  puts "SHOULD NOT REACH"
end
r.resume
r.kill
p r.alive?

# kill on an unstarted fiber: the body never runs.
u = Fiber.new { puts "SHOULD NOT RUN" }
u.kill
p u.alive?

# The kill handling must not disturb ordinary rescue in a fiber body.
n = Fiber.new do
  begin
    raise "normal"
  rescue => e
    Fiber.yield "rescued #{e.message}"
  end
end
p n.resume
