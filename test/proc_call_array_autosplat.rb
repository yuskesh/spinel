# Proc#call with a single Array on a multi-parameter non-lambda proc auto-splats
# the array across the parameters; passing the exact args is unchanged.
fmt = proc { |a, b, c| "#{a}-#{b}-#{c}" }
p fmt.call([1, 2, 3])
p fmt.call(1, 2, 3)
add = proc { |x, y| x + y }
p add.call([4, 5])
p add.call(4, 5)
cat = proc { |a, b| a + b }
p cat.call(["x", "y"])
# A heterogeneous array (element type not a single static kind) auto-splats with
# each param bound as poly, so a non-integer element is not miscompiled.
pair = proc { |a, b| [a, b] }
p pair.call([1, "two"])
