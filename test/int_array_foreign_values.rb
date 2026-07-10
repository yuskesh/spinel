# A homogeneous int array widens when a foreign value arrives through
# unshift/insert/concat (like push already did), a gap left by an
# out-of-range index write reads as nil, and delete of an absent value
# returns nil.
a = [1, 2, 3]
a.unshift(:x)
p a
c = [1, 2]
c.insert(1, :x)
p c
d = [1, 2]
d.concat([:x])
p d
e = [1, 2]
e.unshift(1.5)
p e
b = [1, 2, 3]
b[5] = 9
p b
p b[3]
f = [1, 2]
p f.delete(9)
p f.delete(2)
p f
g = [1, 2]
g.push(:x)
p g
