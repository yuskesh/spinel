# Range#each_slice / #each_cons: the block form, the blockless
# Enumerator form (.to_a), and the .map chain all materialize the
# integer range once and reuse the array emitters (range_enum_redispatch).
p (1..6).each_slice(2).to_a
(1..6).each_slice(2) { |s| p s }
p (1..6).each_slice(2).map { |s| s.sum }
p (1..6).each_cons(3).to_a
(1..5).each_cons(2) { |a, b| puts "#{a}-#{b}" }
p (1..6).each_cons(2).map { |a, b| a * b }
r = (2..9)
p r.each_slice(4).to_a
