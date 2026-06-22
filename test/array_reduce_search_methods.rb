# Array reduction/search coverage: minmax_by (one-pass [min, max] by a block
# key) and bsearch_index (find-minimum binary search returning an index or nil).
# Receivers go through per-type method params so each stays a concrete typed
# array, and each block uses a distinct param name (sibling blocks that reuse a
# name share one typed C local, which would widen to poly).
def ints(a); a; end
def flts(a); a; end
def strs(a); a; end
def polys(a); a; end

# minmax_by: ties keep the first occurrence, matching Ruby
p ints([3, 1, 4, 1, 5]).minmax_by { |a1| -a1 }
p ints([3, 1, 4, 1, 5, 9, 2, 6]).minmax_by { |a2| a2 }
p strs(%w[banana apple cherry kiwi]).minmax_by { |s1| s1.length }
p flts([1.5, 0.25, 3.0]).minmax_by { |f1| f1 }
p polys([1, :b, "cc", 4]).minmax_by { |p1| p1.to_s.length }

# an empty receiver yields [nil, nil] -- a generic array, not the typed kind
p ints([]).minmax_by { |e1| e1 }
p strs([]).minmax_by { |e2| e2.length }
p flts([]).minmax_by { |e3| e3 }
p polys([]).minmax_by { |e4| e4 }

# bsearch_index: index of the first element satisfying the predicate, or nil
p ints([1, 2, 3, 4, 5, 6]).bsearch_index { |b1| b1 >= 4 }
p ints([1, 2, 3, 4, 5, 6]).bsearch_index { |b2| b2 >= 10 }
p ints([1, 3, 5, 7, 9]).bsearch_index { |b3| b3 >= 5 }
p ints([1, 3, 5, 7, 9]).bsearch_index { |b4| b4 >= 0 }

# a poly (true/nil) predicate exercises the poly truthiness path
p ints([1, 2, 3, 4, 5, 6]).bsearch_index { |b5| (b5 >= 4) ? true : nil }
