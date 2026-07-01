# Hash#keys / #values on an Integer=>Integer hash (the IntIntHash variant had no
# keys/values runtime helper: keys link-failed and values was a codegen reject).
def id(x) = x

m = {}
m[1] = 10
m[2] = 20
m[3] = 30
p m.keys
p m.values
p m.keys.sum
p m.values.sum

# through a method receiver (non-literal path)
h = id({5 => 50, 6 => 60})
p h.keys
p h.values

# keys/values compose with array methods
p h.keys.map { |k| k * 2 }
p h.values.max

# single-entry literal int=>int hash
g = {7 => 1}
p g.keys
p g.values

# clear empties it: keys/values stay consistent (empty int arrays)
g.clear
p g.keys
p g.values
p g.keys.sum

# re-populating after clear reflects the new entries in insertion order
g[9] = 90
g[8] = 80
p g.keys
p g.values
