# Hash#to_a / #entries materialize entries as an array of [key, value] pairs,
# preserving insertion order, across every typed-hash variant. Each receiver
# goes through a monomorphic method parameter so the runtime path is exercised
# (not constant-folded) without widening any one helper's parameter to poly.

def sym_int(h)  = h.to_a
def str_int(h)  = h.to_a
def str_str(h)  = h.to_a
def int_str(h)  = h.to_a
def int_int(h)  = h.to_a
def sym_poly(h) = h.to_a
def poly_poly(h) = h.to_a
def sym_int_ents(h) = h.entries

p sym_int({ a: 1, b: 2, c: 3 })
p str_int({ "a" => 1, "b" => 2 })
p str_str({ "a" => "x", "b" => "y" })
p int_str({ 1 => "one", 2 => "two" })
p int_int({ 10 => 100, 20 => 200 })
p sym_poly({ a: 1, b: "two" })
p poly_poly({ 1 => 2, "a" => "b" })
p sym_int_ents({ a: 1 })

# Pairs are real arrays: index them and read back keys and values.
ps = sym_int({ x: 10, y: 20, z: 30 })
p ps.length
p ps.map { |pair| pair[0] }
p ps.map { |pair| pair[1] }
p ps.first[0]
p ps.first[1]
