# Hash#dig must descend into whatever the previous key returned -- an
# Array element, a nested Hash, etc. -- dispatching each sub-key on the
# runtime receiver. Previously a symbol-keyed hash forced every sub-key
# to be looked up as a symbol, so `dig(:a, 1)` on an Array value gave nil.
p({ a: [10, 20] }.dig(:a, 1))
p({ a: { b: 1 } }.dig(:a, :b))
p({ "a" => { "b" => 1 } }.dig("a", "b"))
p({ a: { b: [1, 2, 3] } }.dig(:a, :b, 2))
p({ a: [{ c: 9 }] }.dig(:a, 0, :c))
p({ a: 1 }.dig(:a))
p({ a: [1, 2] }.dig(:a, 5))   # out of range -> nil
p({ a: [10, 20] }.dig(:b, 1)) # missing first key -> nil
