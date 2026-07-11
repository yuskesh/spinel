# Hash#default_proc wraps the stored Hash.new{} block as a first-class Proc
# (nil for a hash without one); calling it drives the original block. Proc
# inspect carries a heap address, so the pin checks nil-ness, callability,
# and introspection rather than the printed form.
h = Hash.new { |hash, k| hash[k] = k.to_s }
p h.default_proc.nil?
dp = h.default_proc
p dp.lambda?
p dp.arity
p h[:x]
p dp.call(h, :y)
p h[:y]
p({ a: 1 }.default_proc)
h2 = { b: 2 }
p h2.default_proc
p h2.default_proc.nil?
hs = Hash.new { |hash, k| hash[k] = k.upcase }
hs["ab"] = "AB0"
p hs["cd"]
p hs.default_proc.nil?
p hs.default_proc.call(hs, "ef")
hp = Hash.new { |hash, k| hash[k] = [k, k] }
hp[1] = [9]
p hp[2]
p hp.default_proc.nil?
