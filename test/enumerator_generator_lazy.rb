# A lazy pipeline over a generator Enumerator streams values one at a time, so an
# infinite generator is bounded by a downstream first(n) / take_while without
# materializing the whole (unbounded) source. Finite generators and materialized
# enumerators lazily evaluate the same way.

# --- infinite generator, bounded by first(n) ---
naturals = Enumerator.new { |y| i = 0; loop { y << i; i += 1 } }
p naturals.lazy.map { |x| x * 2 }.first(3)
p naturals.lazy.select { |x| x.even? }.first(4)
p naturals.lazy.reject { |x| x.even? }.first(3)

# --- chained ops on an infinite generator ---
p naturals.lazy.map { |x| x * x }.select { |x| x.odd? }.first(3)
p naturals.lazy.map { |x| x + 1 }.map { |x| x * 10 }.first(3)

# --- take_while forces to a finite prefix ---
p naturals.lazy.take_while { |x| x < 5 }.to_a
p naturals.lazy.map { |x| x * x }.take_while { |x| x < 30 }.to_a

# --- finite generator: map/select to_a and first ---
finite = Enumerator.new { |y| y << 1; y << 2; y << 3; y << 4 }
p finite.lazy.map { |x| x * 10 }.to_a
p finite.lazy.select { |x| x > 1 }.first(2)
p finite.lazy.map { |x| x * x }.reject { |x| x > 9 }.to_a

# --- generator yielding strings ---
words = Enumerator.new { |y| %w[apple berry cherry date].each { |w| y << w } }
p words.lazy.map { |w| w.upcase }.first(2)
p words.lazy.select { |w| w.length > 4 }.to_a

# --- materialized enumerators lazily too ---
p [1, 2, 3, 4, 5, 6].each.lazy.map { |x| x * 2 }.select { |x| x > 4 }.first(2)
p "abcdef".each_char.lazy.map { |c| c.upcase }.first(3)
