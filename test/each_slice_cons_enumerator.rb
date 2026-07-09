# Blockless Array#each_slice(n) / Array#each_cons(n) return a materialized
# Enumerator: a first-class value supporting to_a / next / peek / size / take /
# first(n). The block and .map/.collect chain forms must keep working too.

# --- blockless: a first-class Enumerator value ---
e = [1, 2, 3, 4, 5].each_slice(2)
p e.class
p e.to_a
p e.size
p e.next
p e.next
p e.peek
p e.next
p [1, 2, 3, 4, 5].each_slice(2).first(2)
p [1, 2, 3, 4, 5].each_slice(2).take(2)

c = [1, 2, 3, 4].each_cons(2)
p c.class
p c.to_a
p c.size

# non-divisible and short receivers
p [1, 2, 3, 4, 5].each_slice(3).to_a
p [1, 2, 3].each_slice(5).to_a
p [1].each_cons(2).to_a
p %w[a b c d e].each_slice(2).to_a

# --- assigned, then materialized ---
slices = [10, 20, 30, 40, 50].each_slice(2)
arr = slices.to_a
p arr.length
p arr

# --- .map / .collect chains (fold-unrolled; must not regress) ---
p [1, 2, 3, 4, 5].each_slice(2).map { |s| s.sum }
p [1, 2, 3, 4, 5].each_slice(2).collect { |s| s.first }
p [1, 2, 3, 4].each_cons(2).map { |a, b| a + b }
p [1, 2, 3, 4].each_cons(2).map { |pair| pair }
p [1, 2, 3, 4].each_cons(2).map { |(a, b)| a * b }
p [1.5, 2.5, 3.5].each_slice(2).map { |s| s.sum }
p %w[a b c].each_slice(2).map { |s| s.join }
p [1, 2, 3, 4].each_cons(2).with_index.map { |w, i| [w, i] }

# --- direct block form (yields each slice / window, returns receiver) ---
acc = []
[1, 2, 3, 4, 5].each_slice(2) { |s| acc << s }
p acc
acc2 = []
[1, 2, 3, 4].each_cons(2) { |w| acc2 << w }
p acc2

# --- argument errors ---
begin
  [1, 2, 3].each_slice(0).to_a
rescue ArgumentError => err
  p err.message
end
begin
  [1, 2, 3].each_cons(0).to_a
rescue ArgumentError => err
  p err.message
end
