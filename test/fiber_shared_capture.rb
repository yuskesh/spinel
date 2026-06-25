# A Fiber (or Enumerator.new) block that assigns an enclosing value-type local
# now shares it: the write reaches the outer scope (a heap cell, like an escaping
# proc captures), so it is no longer silently lost. A captured heap object is
# already shared by pointer, so in-place mutation reaches the outer scope too.

# integer accumulator
acc = 0
f = Fiber.new { acc += 1; Fiber.yield acc }
p f.resume          # 1
p acc               # 1 -- the write propagated

# accumulate across resumes
sum = 0
g = Fiber.new do
  sum += 10
  Fiber.yield sum
  sum += 20
  Fiber.yield sum
end
p g.resume          # 10
p sum               # 10
p g.resume          # 30
p sum               # 30

# poly local reassigned to a different type
v = 0
k = Fiber.new { v = "hi"; Fiber.yield v }
p k.resume          # "hi"
p v                 # "hi"

# heap object: in-place mutation is shared by pointer
s = ""
m = Fiber.new { s << "ab"; Fiber.yield s.length }
p m.resume          # 2
p s                 # "ab"

arr = []
n = Fiber.new { arr << 1 << 2; Fiber.yield arr.length }
p n.resume          # 2
p arr               # [1, 2]

# read-only capture still works
base = 100
r = Fiber.new { Fiber.yield(base + 1); Fiber.yield(base + 2) }
p r.resume          # 101
p r.resume          # 102

# Enumerator.new generator sharing an outer counter
calls = 0
e = Enumerator.new do |y|
  calls += 1
  y << calls
  calls += 1
  y << calls
end
p e.next            # 1
p e.next            # 2
p calls             # 2
