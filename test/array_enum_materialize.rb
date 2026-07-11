# Blockless enumerator materialization: cycle(n), repeated_permutation,
# slice_before/slice_after with a pattern value, and each_entry.
a = [1, 2]
p a.cycle(2).to_a          # [1, 2, 1, 2]
p a.cycle(0).to_a          # []
b = [1, 3, 2, 3, 4]
p b.slice_before(3).to_a   # [[1], [3, 2], [3, 4]]
p b.slice_after(3).to_a    # [[1, 3], [2, 3], [4]]
p ["x", "y", "x"].slice_before("x").to_a  # [["x", "y"], ["x"]]
c = [1, 2, 3]
p c.each_entry.to_a        # [1, 2, 3]
r = []
a.repeated_permutation(2) { |x| r << x }
p r                        # [[1, 1], [1, 2], [2, 1], [2, 2]]
p [5, 6].repeated_permutation(2).to_a  # [[5, 5], [5, 6], [6, 5], [6, 6]]
p a.repeated_permutation(0).to_a       # [[]]
