# hash.map.with_index / hash.each.with_index interpose to_a and ride the
# pair-array chain; a nil-typed block tail (puts) keeps its side effects
# through the boxed collect (it used to be folded to a bare nil).
h = { a: 1, b: 2 }
h.map.with_index { |(k, v), i| puts "#{i}:#{k}" }
h.each.with_index { |(k, v), i| puts "#{i}=#{v}" }
[10, 20].map.with_index { |v, i| puts "#{i}->#{v}" }
