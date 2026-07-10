# str.each_char.with_index (block form) and enum.each.with_object desugar
# into the shapes the chain emitters already serve.
"abc".each_char.with_index { |c, i| print "#{i}#{c}" }
puts
"xy".each_char.with_index(10) { |c, i| print "#{i}:#{c} " }
puts
x = [1, 2, 3].each.with_object([]) { |v, a| a << v * 2 }
p x
h = [1, 2].each.with_object({}) { |v, acc| acc[v.to_s] = v }
p h.size
