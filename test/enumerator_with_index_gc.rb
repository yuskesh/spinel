# A blockless <enum>.with_index materializes [element, index] pairs from the
# source enumerator's snapshot. The source (a temporary `arr.each` /
# `str.each_char` enumerator) must stay rooted while the pairs are built --
# otherwise a collection during the build frees it and reads go out of bounds.
p [10, 20, 30].each.with_index.to_a
p [1, 2, 3, 4].each.with_index(1).to_a
p [5, 6].each.with_index.size
p "abc".each_char.with_index.to_a
p (1..4).to_a.each.with_index.map { |x, i| x * 10 + i }
