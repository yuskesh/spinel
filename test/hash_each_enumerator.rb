# Blockless Hash#each / Hash#each_pair return a materialized Enumerator over the
# hash's [key, value] pairs (insertion order), a first-class value supporting
# to_a / next / peek / size / class. The block form must keep working too.

# --- blockless: a first-class Enumerator over [k, v] pairs ---
e = { a: 1, b: 2, c: 3 }.each
p e.class
p e.to_a
p e.size
p e.next
p e.next
p e.peek
p e.next

p({ a: 1, b: 2 }.each_pair.to_a)

# --- across the key/value storage variants ---
p({ "x" => 1, "y" => 2 }.each.to_a)          # string keys, int values
p({ "a" => "b", "c" => "d" }.each.to_a)      # string keys, string values
p({ 1 => "one", 2 => "two" }.each.to_a)      # int keys, string values
p({ [1] => :a, [2] => :b }.each.to_a)        # poly keys and values
p({ a: [1, 2], b: [3, 4] }.each.to_a)        # symbol keys, array values

# --- built-up local hash ---
h = {}
h[:x] = 10
h[:y] = 20
p h.each.to_a
p h.each.size

# --- assigned enumerator, materialized ---
pairs = { one: 1, two: 2, three: 3 }.each_pair
arr = pairs.to_a
p arr.length
p arr

# --- direct block form (yields |k, v|, returns the receiver) ---
seen = []
{ a: 1, b: 2 }.each { |k, v| seen << [k, v] }
p seen
sum = 0
{ a: 1, b: 2, c: 3 }.each_pair { |_k, v| sum += v }
p sum
