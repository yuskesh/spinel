# A Fiber / proc body that REBINDS a captured heap object (String / Array /
# Hash / object) to a new value now propagates that reassignment to the
# enclosing scope, matching value-type captures. Heap captures ride a typed-
# pointer cell, so `s = "new"` writes through the shared cell. In-place
# mutation of a non-rebound capture (`buf << x`) still works by pointer.

# String rebind
s = "old"
Fiber.new { s = "new"; Fiber.yield }.resume
puts s

# Array rebind
arr = [1]
Fiber.new { arr = [9, 9]; Fiber.yield }.resume
p arr

# Hash rebind
h = {a: 1}
Fiber.new { h = {b: 2, c: 3}; Fiber.yield }.resume
p h

# In-place mutation of a non-rebound capture still propagates by pointer
buf = "x"
Fiber.new { buf << "y"; Fiber.yield }.resume
puts buf

acc = []
Fiber.new { acc << 42; Fiber.yield }.resume
p acc

# Rebind AND in-place mutate the same captured string
t = "a"
Fiber.new { t << "b"; t = t + "c"; Fiber.yield }.resume
puts t

# Object rebind
Point = Struct.new(:x)
pt = Point.new(1)
Fiber.new { pt = Point.new(99); Fiber.yield }.resume
p pt.x

# Two procs sharing a reassigned capture (propagation via side effect, to
# avoid the unrelated array-destructured-proc .call return-typing path).
def make
  v = "old"
  list = [1]
  [proc { v = "new"; list = [9, 9] }, proc { puts v; p list }]
end
setter, reader = make
reader.call
setter.call
reader.call
