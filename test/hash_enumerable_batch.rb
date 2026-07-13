# Hash Enumerable conformance (KieranP #2339,#2341,#2350,#2352)
h = { a: 1, b: 2 }
p h.first                                  # #2350 first pair
p h.first(1)
p h.take(1)
p h.drop(1)
p({}.first)
p h.sort { |x, y| y[1] <=> x[1] }          # #2341 sort with comparator block
p h.sort { |x, y| x[1] <=> y[1] }
p({ "a" => 1, "b" => 2 }.key(2))           # #2352 key on a String-keyed hash
p({ 1 => "x", 2 => "y" }.key("y"))
p({ "a" => 1 }.key(9))
p h.any? { |pair| pair[1] > 1 }            # #2339 solo param is the pair
p h.count { |pair| pair[1] > 1 }
p h.all? { |pair| pair[1] > 0 }
p h.any? { |k, v| v > 1 }                   # two params still bind k, v
