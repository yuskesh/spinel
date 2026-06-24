# Hash#sort_by yields each |key, value| pair and returns the [key, value] pairs
# ordered by the block's value. Each helper is monomorphic so the runtime walk
# is exercised without widening the parameter to poly.

def by_v(h)      = h.sort_by { |k, v| v }
def by_neg_v(h)  = h.sort_by { |k, v| -v }
def by_len(h)    = h.sort_by { |k, v| v.length }
def by_key(h)    = h.sort_by { |k, v| k }
def by_key_s(h)  = h.sort_by { |k, v| k.to_s }
def by_v_str(h)  = h.sort_by { |k, v| v }

p by_v({ a: 3, b: 1, c: 2 })
p by_neg_v({ a: 3, b: 1, c: 2 })
p by_len({ a: "xx", b: "y", c: "zzz" })
p by_key({ 3 => 30, 1 => 10, 2 => 20 })
p by_key_s({ c: 1, a: 2, b: 3 })
p by_v_str({ "a" => 3, "b" => 1 })

# The result is a plain array of pairs.
ps = by_v({ x: 30, y: 10, z: 20 })
p ps.length
p ps.first[0]
p ps.last[0]
