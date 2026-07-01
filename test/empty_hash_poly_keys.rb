# keys/values on an evidence-free empty {} (stays poly) and on a hash read out
# of a poly slot. Previously rejected ("unsupported call keys recv=...ty39").
def id(x) = x

# empty hash literal in a local: keys/values are empty arrays
x = {}
p x.keys
p x.values
p x.empty?
p x.size

# never-populated empty-hash ivar: keys/values still resolve
class Store
  def initialize; @h = {}; end
  def ks; @h.keys; end
  def vs; @h.values; end
end
s = Store.new
p s.ks
p s.vs

# a populated heterogeneous hash boxed through poly still answers keys/values
h = {1 => 2, "a" => 3}
p h.keys
p h.values

# symbol-keyed poly hash
g = {a: 1, b: "x"}
p g.keys
p g.values

# an empty hash that later gets writes is unaffected (refines as before)
m = {}
m[1] = 10
m[2] = 20
p m
