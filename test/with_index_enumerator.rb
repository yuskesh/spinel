# Blockless <enumerator>.with_index(offset) returns a materialized Enumerator
# over the [element, index] pairs, a first-class value supporting to_a / next /
# peek / size. The block and terminal-chain forms must keep working.

# --- blockless: a first-class Enumerator over [element, index] pairs ---
e = [10, 20, 30].each.with_index
p e.class
p e.to_a
p e.size
p e.next
p e.next
p e.peek
p e.next

# --- with an explicit offset ---
o = [10, 20, 30].each.with_index(1)
p o.to_a
p o.next

# --- on a String#each_char enumerator ---
p "abc".each_char.with_index(1).to_a

# --- assigned, then materialized ---
pairs = %w[a b c].each.with_index(10)
arr = pairs.to_a
p arr.length
p arr

# --- existing block form (yields |element, index|) still works ---
p [10, 20, 30].map.with_index { |x, i| [x, i] }
p [10, 20, 30].map.with_index(1) { |x, i| x + i }
seen = []
[10, 20].each.with_index { |x, i| seen << [x, i] }
p seen
p [10, 20, 30].select.with_index { |_x, i| i.even? }

# --- existing terminal / map chains still work ---
p [10, 20, 30].each.with_index.to_a
p [10, 20, 30].each.with_index(1).to_a
p [10, 20].each.with_index(1).map { |x, i| [i, x] }
