# Array#each_with_object with an empty `{}` memo builds a general (boxed
# key/value) hash. The memo param is typed as that hash, so `h[k] = v` inside
# the block mutates it, and the populated hash is returned. Receivers are routed
# through monomorphic per-type helpers to exercise the runtime path.
def ints(x); x; end
def strs(x); x; end
def syms(x); x; end

p ints([1, 2]).each_with_object({}) { |x, h| h[x] = x }
p strs(["a", "bb"]).each_with_object({}) { |w, h| h[w] = w.length }
p syms([:a, :b]).each_with_object({}) { |k, h| h[k] = k.to_s }
p ints([1, 2, 3, 4]).each_with_object({}) { |n, h| h[n.even?] = n }

# the result is an ordinary hash
p ints([1, 2]).each_with_object({}) { |x, h| h[x] = x }.keys

# an empty receiver yields the empty memo
a = [1]
a.pop
p a.each_with_object({}) { |x, h| h[x] = x }

# the `[]` array-seed form is unaffected
p ints([1, 2, 3]).each_with_object([]) { |x, a| a << x * 2 }
