# Hash.new { |h,k| ... } must preserve the key type on inspect: a symbol key
# renders as `a:`, a string key as `"a"=>`, an integer key as `1=>`. The
# default-block hash is polymorphic by nature, so keys round-trip faithfully.
sym = Hash.new { |h, k| h[k] = [] }
sym[:a] << 1
sym[:b] << 2
sym[:a] << 3
p sym

str = Hash.new { |h, k| h[k] = [] }
str["a"] << 1
p str

int = Hash.new { |h, k| h[k] = 0 }
int[1] += 5
int[2] += 7
p int

# accumulator idiom across many keys
acc = Hash.new { |h, k| h[k] = [] }
[:x, :y, :x, :z, :y, :x].each { |s| acc[s] << 1 }
p acc

# routed through a method param (defeats constant folding)
def touch(h, k)
  h[k] << k.to_s
end
routed = Hash.new { |h, k| h[k] = [] }
touch(routed, :a)
touch(routed, :a)
touch(routed, :b)
p routed

# mixed key types collapse to a faithful poly hash
mix = Hash.new { |h, k| h[k] = 0 }
mix[:a] += 1
mix["b"] += 2
mix[3] += 3
p mix

# reading a missing key runs the block and returns its value
counts = Hash.new { |h, k| h[k] = k.to_s * 2 }
p counts[:ab]
p counts["cd"]
