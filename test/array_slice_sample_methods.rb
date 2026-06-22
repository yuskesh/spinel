# Array slicing/sampling coverage: rotate(n), slice(start, len) / [start, len],
# sample(n), and the n-arg min(n)/max(n) forms across the typed-array kinds and
# poly arrays. Receivers are routed through per-type method params so each stays
# a concrete typed array (a shared param would widen to poly), exercising the
# runtime builtin path rather than constant folding.
def ints(a); a; end
def strs(a); a; end
def flts(a); a; end
def polys(a); a; end

# rotate: non-mutating, wraps around, negative counts rotate right
p ints([1, 2, 3, 4, 5]).rotate(2)
p ints([1, 2, 3, 4, 5]).rotate
p ints([1, 2, 3, 4, 5]).rotate(-1)
p strs(%w[a b c d]).rotate(1)
p polys([1, :b, "c", 4]).rotate(2)

# the receiver is left unmodified by the non-mutating rotate
orig = ints([1, 2, 3])
orig.rotate(1)
p orig

# slice(start, len) and the equivalent [start, len]
p ints([1, 2, 3, 4, 5]).slice(1, 3)
p ints([1, 2, 3, 4, 5])[1, 3]
p strs(%w[a b c d e]).slice(2, 2)
p polys([1, :b, "c", 4]).slice(1, 2)

# min(n) / max(n): n smallest ascending, n largest descending
p ints([3, 1, 4, 1, 5, 9, 2]).min(3)
p ints([3, 1, 4, 1, 5, 9, 2]).max(2)
p flts([1.5, 0.5, 2.5]).min(2)
p strs(%w[banana apple cherry]).max(2)

# sample(n): nondeterministic order, so assert on length and (when n >= size)
# the sorted full set, which is deterministic
p ints([1, 2, 3, 4, 5]).sample(2).length
p ints([5, 3, 1, 4, 2]).sample(10).sort
p strs(%w[a b c]).sample(5).sort
