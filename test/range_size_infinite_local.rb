# Range#size with a Float::INFINITY (or endless) bound, reached through a
# local variable: the sole-assignment lookthrough types/serves it like the
# literal receiver (Float Infinity), and the range VALUE materializes its
# infinite end as INTPTR_MAX instead of passing (1.0/0.0) through an
# mrb_int parameter (UB: the converted value was a stale register, so the
# previous range's size leaked through).
a = (1..10); p(a.size)
b = (1..Float::INFINITY); p(b.size)
c1 = (5..); p(c1.size)

# take/first on the looked-through infinite range count from the start
d = (2..Float::INFINITY)
p d.take(4)
p d.first(3)

# exclusive infinite end behaves the same
f = (1...Float::INFINITY); p(f.size)

# membership on a materialized infinite-end range
g = (3..Float::INFINITY)
p g.include?(1000000)
p g.cover?(2)
