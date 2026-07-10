# sort_by with an array-returning block (the multi-key sort idiom) orders
# element-wise through the boxed-key comparator.
p [[1, 2], [1, 1], [0, 5]].sort_by { |a, b| [a, b] }
p ["bb", "a", "ccc"].sort_by { |s| [s.length, s] }
p [["b", 2], ["a", 2], ["a", 1]].sort_by { |x, y| [x, y] }
p [3, 1, 2].sort_by { |x| -x }
rows = [{ n: "z", v: 1 }, { n: "a", v: 1 }]
p rows.sort_by { |r| [r[:v], r[:n]] }.map { |r| r[:n] }
