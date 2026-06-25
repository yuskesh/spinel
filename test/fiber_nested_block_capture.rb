# A capture assigned inside a NESTED block within a Fiber / proc body now
# propagates to the enclosing scope. Capture detection (a_collect_used /
# proc_collect_used) descends into nested blocks, so an enclosing local
# written there is celled, while the nested block's own params/locals stay
# block-local. Covers value (Integer), heap rebind (String / Array), and the
# proc form.

# Integer accumulated inside a nested block
acc = 0
Fiber.new do
  3.times { |i| acc += i + 1 }
  Fiber.yield
end.resume
p acc                       # 6

# String rebound inside a nested block
total = "x"
Fiber.new do
  2.times { |i| total = total + i.to_s }
  Fiber.yield
end.resume
puts total                  # x01

# Array rebound (literal) inside a nested block
out = [0]
Fiber.new do
  [10, 20].each { |n| out = [n, n] }
  Fiber.yield
end.resume
p out                       # [20, 20]

# Nested block's own param/local is not mistaken for a capture
seen = 0
Fiber.new do
  [5, 6].each { |x| tmp = x * 2; seen += tmp }
  Fiber.yield
end.resume
p seen                      # 22

# proc capturing through a nested block (propagation via side effect)
def make
  n = 0
  bump = proc { 4.times { |k| n += k } }
  [bump, proc { n }]
end
bump, get = make
bump.call
p get.call                  # 6
