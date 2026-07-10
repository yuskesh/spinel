# Splat into the print builtins and format expands the array elements.
a = [1, 2, 3]
puts(*a)
print(*a)
puts
p(*a)
args = ["%d-%d", 3, 4]
puts format(*args)
b = ["x", :y, [7, 8]]
puts(*b)
