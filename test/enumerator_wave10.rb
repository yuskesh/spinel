# lazy filter_map / flat_map (finite + infinite sources, array sources)
p([1, 2, 3, 4].lazy.filter_map { |x| x }.to_a)
p([1, 2, 3].lazy.flat_map { |x| [x, x] }.to_a)
p((1..).lazy.flat_map { |x| [x, -x] }.first(5))
p((1..).lazy.filter_map { |x| x * 2 if x.odd? }.first(4))
p([1, 2, 3, 4].lazy.filter_map { |x| x if x.even? }.to_a)
p([[1, 2], [3]].lazy.flat_map { |a| a }.to_a)
# Enumerator#inspect / class
e = [1, 2, 3].each
p(e)
p e.inspect
p [4, 5].reverse_each
p((1..3).each)
p [1, 2, 3].each_slice(2)
p((1..3).each.class)
# stored-enumerator with_index / with_object
ei = [1, 2, 3].each
ei.with_index(10) { |x, i| p [x, i] }
ei2 = [1, 2, 3].each
ei2.with_index { |x, i| p [x, i] }
eo = [4, 5].each
p eo.with_object([]) { |x, acc| acc << x * 2 }
p eo.with_object({}) { |x, h| h[x] = x * x }
