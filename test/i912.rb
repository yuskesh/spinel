h = Hash.new { |hash, key| "missing: #{key}" }
puts h[:a]
puts h[:b]
h[:c] = "set"
puts h[:c]
puts h[:d]

counts = Hash.new { |hsh, k| 0 }
counts[:x] = 5
puts counts[:x]
puts counts[:y]

d = Hash.new { |hsh, key| key.to_s.length }
puts d["hello"]
puts d["hi"]
