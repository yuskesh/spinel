# Stress test: bigint propagation through a chain of dependent locals.
# `a` starts int but is promoted to bigint by `a = a * 2` inside the
# while loop (detected by scan_bigint_in_loop). Subsequent assignments
# `b = a + 1`, `c = b + 1`, ... force scan_bigint_propagate to walk
# the dependency chain over multiple fixed-point iterations. Verifies
# the loop converges far below the 256-iteration safety cap and that
# the oscillation guard does not trigger on legitimate monotonic
# propagation.

a = 1
i = 0
while i < 50
  a = a * 2
  i = i + 1
end

b = a + 1
c = b + 1
d = c + 1
e = d + 1
f = e + 1
g = f + 1
h = g + 1

puts a > 0
puts b > 0
puts c > 0
puts d > 0
puts e > 0
puts f > 0
puts g > 0
puts h > 0
puts (h - a) == 7
