# Hash#sort returns the entries as an array of [key, value] pairs ordered by
# Array#<=> (key first, then value). Each receiver goes through a monomorphic
# method parameter so the runtime path is exercised, not constant-folded.

def sym_keyed(h)  = h.sort
def str_keyed(h)  = h.sort
def int_keyed(h)  = h.sort
def int_int(h)    = h.sort

p sym_keyed({ b: 2, a: 1, c: 3 })
p str_keyed({ "d" => 1, "a" => 3, "c" => 2 })
p int_keyed({ 3 => "x", 1 => "z", 2 => "y" })
p int_int({ 30 => 1, 10 => 3, 20 => 2 })

# Equal first elements fall back to comparing the second.
p int_int({ 10 => 5, 5 => 9, 10 => 2 })

# Result is a plain array of pairs.
ps = sym_keyed({ z: 1, a: 2 })
p ps.length
p ps.first[0]
p ps.last[0]
