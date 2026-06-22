# Array#unshift prepends its arguments and returns the (mutated) receiver.
# The return value was lost (nil) for the assigned/expression form, and the
# float-array case did not actually prepend. Covers int, string, and float
# arrays, multi-argument unshift (argument order preserved), the statement
# form, and a non-literal receiver.
x = [2, 3].unshift(1)
p x
a = ["x", "y"]
p a.unshift("z")
b = [1.0, 2.0]
p b.unshift(0.5)
p [2, 3].unshift(1, 0)

q = [1, 2]
q.unshift(0)
p q

def prepend9(arr)
  arr.unshift(9)
end
p prepend9([1, 2])

# Multi-argument unshift evaluates its arguments left to right, even though
# they are prepended in reverse to keep argument order.
order = []
mk = ->(v) { order << v; v.to_f }
fa = [9.0]
fa.unshift(mk.call(1), mk.call(2))
p fa
p order
