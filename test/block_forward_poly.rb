# Forwarding a callable value into poly-element collections. Now that poly
# values round-trip through proc parameters, &proc / &lambda / &:sym / &method
# forward into poly arrays, and a Method object forwards into Hash#each (the
# [k, v] pair is passed as a single array argument through the array ABI).
# Distinct per-type helpers keep the receiver element types monomorphic.
def poly(a) = a
def ints(a) = a

mixed = poly([1, "two", :three, 4.5])

# proc / lambda / sym / method values into a poly-array map
dbl = ->(x) { [x, x] }
p mixed.map(&dbl)                 # [[1, 1], ["two", "two"], [:three, :three], [4.5, 4.5]]
p mixed.map(&:to_s)              # ["1", "two", "three", "4.5"]
def insp(x) = x.inspect
p mixed.map(&method(:insp))      # ["1", "\"two\"", ":three", "4.5"]

# select over a poly array with a predicate proc value
is_int = ->(x) { x.is_a?(Integer) }
p mixed.select(&is_int)          # [1]

# a forwarded proc with a non-int (String) return over an int array
nums = ints([1, 2, 3])
label = ->(x) { "n#{x}" }
p nums.map(&label)               # ["n1", "n2", "n3"]

# hash each / each_key / each_value forwarded to a Method object
def show(pair) = p(pair)
def showk(k) = p(k)
def showv(v) = p(v)
h = { a: 10, b: 20 }
h.each(&method(:show))           # [:a, 10] / [:b, 20]
h.each_key(&method(:showk))      # :a / :b
h.each_value(&method(:showv))    # 10 / 20
