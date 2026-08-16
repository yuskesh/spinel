# The array a loop is walking must survive the loop's own body.
#
# `(1..n).each_slice(2) { ... }`, `each_cons`, `grep` and friends all
# materialise the receiver into a plain C temporary (a Range lowers to
# sp_range_to_ia(), a chained map/select hands over its own result) and then
# read the loop bound and every element back out of it. Nothing else holds
# that array, and the body allocates on every iteration, so a collection
# inside the loop sweeps the array being walked: the bound read comes back
# out of the recycled block and the walk stops early or yields the wrong
# elements. The accumulator and the per-iteration row are rooted; the source
# was the one slot left out (E023).
#
# The allocating body is what makes this reproduce at the DEFAULT collection
# threshold rather than only under SPINEL_GC_STRESS -- the body has to
# allocate past the threshold while the source is still unrooted -- and the
# drifting pad sweeps the phase so the collection lands inside the loop
# rather than beside it.

# 1. statement-form each_slice over a Range
sum_slice = 0
(1..2000).each_slice(2) do |s|
  pad = "x" * 400
  sum_slice += s.sum + pad.length
end

# 2. statement-form each_cons over a Range
sum_cons = 0
(1..2000).each_cons(2) do |a, b|
  pad = "y" * 400
  sum_cons += a + b + pad.length
end

# 3. grep over a Range that a heap-filling loop keeps under pressure
r = (1..4000)
held = []
10.times { |i| held << "z" * (40_000 + i) }
got_grep = r.grep(2..4)

p sum_slice != 2_401_000
p sum_cons != 4_799_599
p got_grep != [2, 3, 4]
p held.length != 10
