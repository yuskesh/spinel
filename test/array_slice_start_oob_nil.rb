# Array#[start, len] / #slice(start, len) with start outside [-len, len]
# returns nil like CRuby (start == len is the empty slice; a negative
# length is nil) -- on typed int/str arrays and poly arrays, through both
# the nil-capable and the plain slice arms.
a = [1, 2, 3]
p a[3, 1]
p a[4, 1]
p a[-4, 1]
p a[3, 0]
p a[2, -1]
p a.slice(3, 1)
p a.slice(5, 2)
s = ["x", "y"]
p s[2, 1]
p s[3, 1]
p s[2, 0]
m = [1, "a", :b]
p m[3, 1]
p m[4, 2]
p m[3, 0]
