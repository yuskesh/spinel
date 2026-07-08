# Hash#except returns a copy of the hash without the given keys.

# 1. Symbol-keyed, drop one / several / a missing key.
p({ a: 1, b: 2, c: 3 }.except(:b))    #=> {a: 1, c: 3}
p({ a: 1, b: 2, c: 3 }.except(:a, :c))#=> {b: 2}
p({ a: 1, b: 2 }.except(:z))          #=> {a: 1, b: 2}
p({ a: 1 }.except)                    #=> {a: 1}

# 2. String-keyed.
p({ "x" => 1, "y" => 2 }.except("x")) #=> {"y" => 2}
h = { "a" => 1, "b" => 2, "c" => 3 }
p h.except("b")                       #=> {"a" => 1, "c" => 3}

# 3. Through a method param (defeats constant-folding).
def drop(h) = h.except(:b)
p drop({ a: 1, b: 2, c: 3 })          #=> {a: 1, c: 3}

# 4. The receiver is not mutated.
orig = { a: 1, b: 2 }
orig.except(:a)
p orig                                #=> {a: 1, b: 2}

# 5. Key supplied as a poly method param (coerced, not a literal).
def drop_sym(k) = { a: 1, b: 2, c: 3 }.except(k)
p drop_sym(:b)                        #=> {a: 1, c: 3}
def drop_str(k) = { "a" => 1, "b" => 2 }.except(k)
p drop_str("a")                       #=> {"b" => 2}

puts "done"
