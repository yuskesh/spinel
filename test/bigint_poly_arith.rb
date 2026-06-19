# Arithmetic on a poly value that holds a bigint: a boxed bigint operand used to
# fall through sp_poly_add/sub/mul to 0; now it dispatches to bigint arithmetic.
b = 1
i = 0
while i < 70
  b = b * 2
  i = i + 1
end
arr = [b, "x"]      # heterogeneous -> arr[0] is poly holding a bigint
x = arr[0]
puts(x + 1)
puts(x - 1)
puts(x * 2)
puts(x + x)
