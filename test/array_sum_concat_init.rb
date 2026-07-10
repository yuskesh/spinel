# Array#sum with a String or Array initial value folds by concatenation.
p ["a", "b", "c"].sum("")
p ["b", "c"].sum("a:")
p [[1], [2], [3]].sum([])
p [[1], [2]].sum([0])
p [1, 2, 3].sum(0.5)
p [1, 2, 3].sum
