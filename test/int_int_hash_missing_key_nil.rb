h = {1 => 10, 2 => 20}
p h[99]
p h[99].nil?
p h[99].inspect
p(h[99] || 42)
v = h[99]
p v.nil?
p(v || 7)
p h[1]
p(h[1] + 1)
