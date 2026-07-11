# Rightward pattern assignment (`expr => pattern`) with splats, posts, and
# **rest captures, plus one-line `in` hash patterns carrying value
# sub-patterns (literals, ranges, alternations).

# rightward array destructure through a method boundary
def pair_sum(x)
  x => [a, b]
  a + b
end
p pair_sum([3, 4])

# middle splat: required, splat, post
arr = [1, 2, 3, 4, 5]
arr => [first, *middle, last]
p [first, middle, last]

# empty middle
[10, 20] => [lo, *mid, hi]
p [lo, mid, hi]

# splat with two posts
[1, 2, 3, 4] => [head, *body, y2, z2]
p [head, body, y2, z2]

# splat over a string array keeps the element kind
%w[red green blue] => [c1, *cs]
p c1
p cs

# trailing-comma implicit rest and unnamed splat: length is a floor (the extra
# elements are absorbed), and neither binds a rest target
[1, 2, 3] => [only,]
p only
[1, 2, 3] => [head2, *]
p head2

# too-short input raises NoMatchingPatternError
begin
  [1] => [p1, *ps, p9]
  puts "matched"
rescue NoMatchingPatternError => e
  puts e.class
end

# **rest captures the unlisted pairs
h = { name: "a", x: 1, y: 2 }
h => { name:, **rest }
p name
p rest

# **rest is empty when every key is listed
{ k: 5 } => { k:, **left }
p k
p left

# one-line `in` with value literals
q = { a: 1 }
pr = (q in { a: 1 }); p pr
pr = (q in { a: 2 }); p pr
pr = (q in { b: 1 }); p pr

# range and alternation value sub-patterns
r = { n: 7 }
pr = (r in { n: 1..9 }); p pr
pr = (r in { n: 10..20 }); p pr
pr = (r in { n: 3 | 7 | 11 }); p pr
pr = (r in { n: 4 | 8 }); p pr

# string-valued
s = { tag: "hot" }
pr = (s in { tag: "hot" }); p pr
pr = (s in { tag: "cold" }); p pr

# class-valued keys still match
pr = (q in { a: Integer }); p pr
pr = (s in { tag: Integer }); p pr
