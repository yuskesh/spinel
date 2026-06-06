h = {a: 1, b: 2}
p h[:miss]
p h[:miss].nil?
p h[:miss].inspect
p(h[:miss] || 42)
v = h[:miss]
p v.nil?
p(v || 7)
p h[:a]
p(h[:a] + 1)
# Hash.new(N) keeps its explicit default; {} default is nil.
c = Hash.new(0)
p c[:z]
p({}.default)
p c.default
