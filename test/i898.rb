# Random uses a different RNG than CRuby, so values differ; the test
# checks the interface (types, ranges, reproducibility), not exact
# sequence parity. bytes#length is avoided because spinel has no
# ASCII-8BIT encoding — String#length counts UTF-8 chars, so a random
# byte buffer's char-count is not its byte-count.
r = Random.new(42)
puts r.rand(100).class
puts r.rand.class
puts (r.rand(100) >= 0 && r.rand(100) < 100)
puts r.bytes(4).class

# Same seed -> same stream.
a = Random.new(7)
b = Random.new(7)
puts (a.rand(1000000) == b.rand(1000000))

# Different seeds -> (almost surely) different draws.
c = Random.new(1)
d = Random.new(2)
puts (c.rand(1000000) != d.rand(1000000))

# Class-method rand form.
puts Random.rand(100).class
