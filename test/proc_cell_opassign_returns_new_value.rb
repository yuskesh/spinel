# A stored proc/lambda whose body op-assigns a captured local must return the
# POST-assignment value (not the stale pre-assignment slot), while the mutation
# still persists in the captured variable.
n = 5
c = -> { n += 10 }
p c.call
p n

m = 2
d = proc { m *= 3 }
p d.call
p m

# an inline (non-escaping) block accumulator is a separate path; keep it covered.
acc = 0
[1, 2, 3].each { acc += 1 }
p acc
