# Same chained-`||=` lowering, but for `str_poly_hash` instead of
# `sym_poly_hash`. Exercises the `const char *` key path (vs the
# `sp_sym` integer key path) of the typed-poly-hash branch added
# alongside the sym_poly_hash arm.

class C
  def initialize
    @h = {}
    @h["init"] = []      # forces str_poly_hash
  end
  def add(k, v)
    (@h[k] ||= []) << v
  end
  def count(k); @h[k].length; end
end

c = C.new
c.add("init", 7)         # appends to existing
c.add("foo", 1)
c.add("foo", 2)
c.add("bar", 100)
puts c.count("init")     # 1
puts c.count("foo")      # 2
puts c.count("bar")      # 1
