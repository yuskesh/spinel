# Blockless Hash#each_value / #each_key return an Enumerator, so .to_a and
# .map chains compose (each/each_pair already did).
h = { a: 1, b: 2 }
p h.each_value.to_a
p h.each_key.to_a
p h.each_value.map { |v| v * 2 }
p h.each_key.map { |k| k.to_s }
sh = { "x" => 10 }
p sh.each_value.to_a
p sh.each_key.to_a
