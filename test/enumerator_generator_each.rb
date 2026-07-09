# A fiber-backed generator Enumerator (Enumerator.new { |y| ... }) and a
# materialized enumerator both drive a block via #each: each drains the source
# once and yields every value. next/peek/to_a/first/take/rewind already worked;
# this adds the eager block form.

# --- generator #each over integers ---
gen = Enumerator.new { |y| y << 1; y << 2; y << 3 }
collected = []
gen.each { |x| collected << x }
p collected

# --- generator #each over strings ---
Enumerator.new { |y| y << "a"; y << "b" }.each { |s| puts s }

# --- generator whose body loops a bounded number of times ---
squares = Enumerator.new { |y| 4.times { |i| y << i * i } }
sum = 0
squares.each { |n| sum += n }
p sum

# --- generator yielding arrays, destructured in the block ---
pairs = Enumerator.new { |y| y << [1, 10]; y << [2, 20]; y << [3, 30] }
pairs.each { |k, v| p [k, v, k + v] }

# --- y.yield form drives #each too ---
via_yield = Enumerator.new { |y| y.yield(100); y.yield(200) }
via_yield.each { |n| p n }

# --- the eager terminals still work alongside #each ---
e = Enumerator.new { |y| y << 1; y << 2; y << 3 }
p e.next
p e.next
e.rewind
p e.to_a
p e.first(2)
p e.take(2)

# --- an infinite generator is still bounded by first/take (no hang) ---
naturals = Enumerator.new { |y| i = 0; loop { y << i; i += 1 } }
p naturals.first(5)
p naturals.take(3)

# --- materialized enumerators drive #each as well ---
["x", "y", "z"].each.each { |c| print c }
puts

got = []
[10, 20, 30].each.each { |n| got << n * 2 }
p got

"hi".each_char.each { |ch| p ch }
