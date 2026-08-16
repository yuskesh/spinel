# A hash operand of Hash#merge must survive the merge's own allocation.
#
# `h.merge(a).merge(b)` -- which is also what a multi-argument merge lowers to
# -- leaves the inner merge's freshly allocated result in a C temporary with no
# GC root, and the outer merge's first statement allocates. A collection there
# reclaims it and the allocator hands the same block straight back, zeroed, so
# the receiver's entries silently vanish. The same holds for the cross-variant
# promotions (both sides allocate, C leaves their order unspecified) and for
# two freshly built sibling operands.
#
# The loop and the drifting `pad` are what make this reproduce at the DEFAULT
# collection threshold: one merge chain only breaks when a collection happens
# to land in the few instructions between the inner merge returning and the
# outer merge allocating, so the phase has to be swept rather than sampled.
h1 = { a: 1 }
g1 = { "x" => 1 }

def mk(v)
  { a: v, b: v }
end

lost_chain = 0
lost_multi = 0
lost_cross = 0
lost_sib = 0
i = 0
while i < 20_000
  pad = Array.new(i % 11) { |z| [z] }
  lost_chain += 1 if h1.merge({ b: 2 }).merge({ c: 3 }).size != 3
  lost_multi += 1 if h1.merge({ b: 2 }, { c: 3 }).size != 3
  lost_cross += 1 if g1.merge({ "y" => "s" }).size != 2
  r = mk(1).merge(mk(2))
  lost_sib += 1 if r[:a] != 2 || r[:b] != 2
  i += 1
end

p lost_chain > 0
p lost_multi > 0
p lost_cross > 0
p lost_sib > 0
