# Kernel#pp prints inspect like p (and returns its argument); Hash#store is
# the method form of []=; an out-of-range float literal is Float::INFINITY
# (it used to emit an invalid C token).
pp [1, 2, 3]
x = pp(42)
puts x
h = {}
h.store(:x, 5)
p h[:x]
sh = {}
sh.store("k", "v")
p sh["k"]
p 1e400
p(-1e400)
p 1e308
