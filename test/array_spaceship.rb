# Array#<=>: lexicographic element-wise comparison with a length tiebreak,
# across every array kind. Receivers go through per-type method params so each
# stays a concrete typed (or boxed-poly) array rather than constant folding.
# A typed comparison whose elements are mutually incomparable yields nil.
def ints(a); a; end
def strs(a); a; end
def flts(a); a; end
def polys(a); a; end

# element difference decides before length
p(ints([1, 2, 3]) <=> ints([1, 2, 4]))
p(ints([1, 2, 4]) <=> ints([1, 2, 3]))
p(ints([1, 2, 3]) <=> ints([1, 2, 3]))

# equal prefix: the shorter array is smaller
p(ints([1, 2, 3]) <=> ints([1, 2]))
p(ints([1, 2]) <=> ints([1, 2, 3]))

# string and float arrays compare the same way
p(strs(%w[a b]) <=> strs(%w[a c]))
p(strs(%w[a b c]) <=> strs(%w[a b]))
p(flts([1.0, 2.0]) <=> flts([1.0, 2.0, 3.0]))
p(flts([2.5]) <=> flts([1.5]))

# poly arrays (mixed element kinds), passed through a method param
p(polys([1, "a"]) <=> polys([1, "b"]))
p(polys([1, 2]) <=> polys([1, 2, 3]))

# mutually incomparable element kinds: nil
p(ints([1, 2]) <=> strs(["a"]))

# an array compared to itself is 0; a self-referential array still terminates
a = ints([1, 2, 3])
p(a <=> a)
r = polys([1, 2])
r.push(r)
p(r <=> r)
