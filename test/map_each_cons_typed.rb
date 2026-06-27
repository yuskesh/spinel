# Typed block results through each_cons(n).map and each_cons(n).with_index.map
# must collect the real values. Guards the emit_boxed refactor of these
# collecting emitters against regressing the typed (non-poly) path.
p [1, 2, 3, 4].each_cons(2).map { |a, b| a + b }
p [1, 2, 3, 4].each_cons(2).map { |pair| pair.sum }
p %w[a b c].each_cons(2).map { |x, y| "#{x}#{y}" }
p [10, 20, 30].each_cons(2).with_index.map { |pair, i| pair.sum + i }
