# A poly-typed integer (a heterogeneous-array element) whose arithmetic
# overflows int64 auto-promotes to bigint under --int-overflow=promote.
arr = [4000000000000000000, "x"]
x = arr[0]
puts(x * 3)
puts(x + x + x)
y = arr[0]
puts(y * y)
