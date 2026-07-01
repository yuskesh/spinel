# Splatting a poly value into an array literal: whether it holds an array is
# only known at runtime. An array is spliced one level, nil contributes nothing,
# and any other scalar is inserted as-is (CRuby splat semantics). The receiver
# `x` is a genuinely poly method parameter (called with arrays and scalars).
def g(x)
  [:a, *x, :b]
end

p g([1, 2])        # [:a, 1, 2, :b]
p g(nil)           # [:a, :b]
p g(7)             # [:a, 7, :b]
p g([])            # [:a, :b]
p g(["s", :y])     # [:a, "s", :y, :b]
p g(%i[foo bar])   # [:a, :foo, :bar, :b]  (poly holding a SymArray)

# poly obtained via multiple assignment (a[0] is a poly array element)
a = [[:x, :y], 5]
pat, pr = a
p [:begin, *pat, pr]   # [:begin, :x, :y, 5]
