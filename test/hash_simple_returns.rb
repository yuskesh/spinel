# Hash: clear returns the emptied hash; no-arg merge is a copy; no-arg slice
# is an empty hash; blockless one? is exactly-one-pair; to_hash is self.
h = { a: 1, b: 2 }
r = h.clear
p r
p h
p({ a: 1, b: 2 }.merge)
p({ a: 1, b: 2 }.slice)
p({ a: 1 }.one?)
p({ a: 1, b: 2 }.one?)
p({}.one?)
p({ a: 1 }.to_hash)
g = { "x" => 1 }
p g.merge
p g.one?
