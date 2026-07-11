# CRuby auto-splat of a single yielded Array across multiple block params,
# and Kernel#String on containers/ranges (returns the object's to_s).
def pairs124; yield [1, 2]; end
pairs124 { |a, b| p(a + b) }
def trip; yield [3, 4, 5]; end
trip { |a, b, c| p [a, b, c] }
trip { |a, b| p [a, b] }
def strs; yield ["x", "y"]; end
strs { |a, b| p(a + b) }
def one; yield [7]; end
one { |a, b| p [a, b] }
def three; yield [1, 2, 3]; end
three { |a, *r| p [a, r] }
three { |*r| p r }
def two_args; yield 1, 2; end
two_args { |a, b| p [a, b] }
def poly_arr; yield [1, "x"]; end
poly_arr { |a, b| p [a, b] }
p(String([1, 2]))
p(String(123))
p(String(:sym))
p(String(1.5))
p(String(nil))
p(String("s"))
p(String({ a: 1 }))
p(String(1..3))
