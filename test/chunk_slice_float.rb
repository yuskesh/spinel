# Block-based slicing/chunking materialized via .to_a on Float-element
# arrays and on an empty (untyped) array literal.

p([1.0, 2.0, 4.0, 5.0].slice_when { |x, y| y - x > 1.0 }.to_a)
p([1.0, 2.0, 4.0].chunk_while { |x, y| y - x <= 1.0 }.to_a)
p([1.0, 2.0, 4.0].slice_before { |x| x > 3.0 }.to_a)
p([1.0, 2.0, 4.0].slice_after { |x| x > 1.5 }.to_a)
p([1.0, 1.0, 2.0].chunk { |x| x }.to_a)
p([].chunk { |x| x }.to_a)
p([].slice_when { |x, y| x < y }.to_a)
