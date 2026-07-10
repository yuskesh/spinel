# Hash#slice picks the given keys into a fresh hash (missing keys skipped).
p({ a: 1, b: 2, c: 3 }.slice(:a, :b))
p({ a: 1 }.slice(:zz))
h = { "x" => 1, "y" => 2 }
p h.slice("y")
p({ a: 1, b: 2 }.except(:a))
