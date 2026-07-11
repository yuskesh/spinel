# with_index on a STORED Enumerator with its return value captured: the
# block form drains the snapshot, drives the block with the offset index,
# and returns the enumerator's each return -- the source receiver for an
# each-family enumerator. (The discarded-return statement form and the
# immediate arr.each/map.with_index chains keep their own emitters.)
e = [1, 2, 3].each
v = e.with_index(10) { |x, i| [x, i] }
p v
acc = []
w = e.with_index { |x, i| acc << [x, i] }
p w
p acc
# chain forms keep their own typing/emitters
m = [4, 5].map.with_index(1) { |x, i| x * i }
p m
r = [7, 8].each.with_index { |x, i| acc << x + i }
p r
