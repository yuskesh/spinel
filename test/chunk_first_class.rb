# int_array.chunk { |x| key }.to_a returns a first-class array of [key, [members]]
# pairs (consecutive equal keys grouped), so p/indexing/iteration work.
p [1, 1, 2, 3, 3].chunk { |x| x }.to_a
p [1, 2, 3, 4].chunk { |x| x.even? ? 1 : 0 }.to_a
p [1, 1, 1].chunk { |x| x }.to_a
p [5].chunk { |x| x }.to_a
# the result is a real array: destructure pairs and chain
p [1, 1, 2, 2, 2, 3].chunk { |x| x }.to_a.map { |k, grp| [k, grp.sum] }
p [1, 1, 2, 3, 3].chunk { |x| x }.to_a.length
# the existing inspect form still works
p [1, 1, 2].chunk { |x| x }.to_a.inspect
