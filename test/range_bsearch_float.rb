# Range#bsearch over a float-bounded range bisects the real interval (a
# float member, or nil) -- previously it crashed with "can't iterate from
# Float". Integer ranges keep their index bisection.
p((0.1...2.3).bsearch { |x| x > 1.0 })
p((0.0..10.0).bsearch { |x| x >= 4.0 })
p((0.1...2.3).bsearch { |x| x > 3 })
p((-5.0..5.0).bsearch { |x| x >= 0 })
p((1..10).bsearch { |x| x >= 4 })
p((1..10).bsearch { |x| x > 100 })
p((0..8).bsearch { |x| x * x >= 33 })
