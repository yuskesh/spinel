# x stays poly (called with String and Symbol), so #to_sym dispatches through
# the poly-scalar runtime path rather than a concrete String/Symbol path.
def as_sym(x) = x.to_sym

p as_sym("hello")
p as_sym(:already)
p as_sym("with spaces")

# map over a heterogeneous array whose elements are strings and symbols.
p ["a", :b, "c", :d].map(&:to_sym)

# a poly value that is neither String nor Symbol raises NoMethodError.
def try_sym(x)
  x.to_sym
rescue NoMethodError => e
  "NoMethodError: #{e.message}"
end
puts try_sym("ok")
puts try_sym(42)
