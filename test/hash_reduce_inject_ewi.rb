# Hash#reduce / #inject / #each_with_index materialize the [k, v] pairs once
# and re-dispatch through the array emitters (the same shape the range
# redispatch uses). The |acc, (k, v)| destructure binds the pair's leaves.
h = { a: 1, b: 2, c: 3 }
p h.reduce(0) { |sum, (k, v)| sum + v }
p h.inject(0) { |sum, (k, v)| sum + v }
p h.inject(100) { |sum, (k, v)| sum + v }
h.each_with_index { |(k, v), i| puts "#{i}:#{k}=#{v}" }
sh = { "x" => 5, "y" => 7 }
p sh.reduce(0) { |s, (k, v)| s + v }
p(({ a: 2 }).reduce(10) { |s, (k, v)| s * v })
p [[:a, 1], [:b, 2]].reduce(0) { |s, (k, v)| s + v }
