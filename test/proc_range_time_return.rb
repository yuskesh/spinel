# A proc/lambda returning a Range or Time (by-value structs that don't fit the
# mrb_int proc-ABI carrier) boxes through the poly return slot; the call site
# dereferences the boxed heap copy back to the value. Step-ranges (downto)
# carry their direction through the box.
f = ->(n) { (1..n) }
r = f.call(3)
p r
p r.to_a
p f.call(5).sum
g = proc { |a, b| (a...b) }
p g.call(2, 6).to_a
h = ->() { Time.at(0) }
t = h.call
p t.class
p t.to_i
make = ->(n) { n.downto(1) }
p make.call(3).to_a
