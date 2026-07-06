# frozen_string_literal: true
# A frozen source literal carries a real string header (the 0xf1 marker
# promises one for the hash cache); baked onto bare rodata the cached-hash
# read landed in neighboring rodata, so a literal Hash key missed entries
# whose equal-content keys were built at runtime.
h = {}
"apple,banana,cherry".split(",").each { |w| h[w] = w.length }
literal_key = "banana"
puts h[literal_key]
puts h[literal_key.dup]
p "banana".frozen?
begin
  literal_key << "!"
rescue FrozenError, RuntimeError
  puts "frozen enforced"
end
