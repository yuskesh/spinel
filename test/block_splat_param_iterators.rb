# Block splat parameters through the specialized iterator lowerings: a
# splat-only block wraps each yielded element whole (two-value yields pack
# both); |x, *r| binds an empty rest for scalar elements. Unsupported
# lowerings reject at compile time rather than binding nil.
def m1(a) = a.map { |*x| x }
p m1([1, 2])
def m2(a) = a.map { |*x| x }
p m2(["a", "b"])
def m3(a) = a.map { |*x| x }
p m3([1, :s])
def s1(a) = a.select { |*x| x[0] > 1 }
p s1([1, 2])
def r1(a) = a.reject { |*x| x[0] > 1 }
p r1([1, 2])
def e1(a); acc = []; a.each { |*x| acc << x }; acc; end
p e1([7, 8])
def t1(n); acc = []; n.times { |*x| acc << x }; acc; end
p t1(2)
def mr(a) = a.map { |x, *r| [x, r] }
p mr([1, 2])
def ewi(a); acc = []; a.each_with_index { |*x| acc << x }; acc; end
p ewi([9, 10])
# A boxed (sp_RbVal) poly receiver -- `cond ? array : hash` -- reaches the poly
# map fold, which previously bound only a required param and left `*x` empty
# (nil elements). It now wraps the whole element into the rest array too.
def pm(c); (c ? [1, 2, 3] : { a: 1 }).map { |*x| x[0] }; end
p pm(true)
# and stable when the poly value is rebuilt through the map across a loop
def pl; r = [1, 2, 3]; 3.times { r = r.map { |*x| x[0] } }; r; end
p pl
