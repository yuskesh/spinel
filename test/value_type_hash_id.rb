a = Complex(2, 3)
p(a.hash == a.hash)
p(a.hash == Complex(2, 3).hash)
r = (1..3)
p(r.hash == r.hash)
p a.object_id.is_a?(Integer)
require 'set'
s = Set[1, 2, 3]
p(s.hash == s.hash)
class C; end
c = C.new
p(c.hash == c.hash)
