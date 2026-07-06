# Boxed hashes compare by value like every other container: the poly-eq
# object arm had no hash cases, so [h] == [same-content-h] was pointer
# identity and always false.
h1 = { "foo" => :bar }
p [h1] == [{ "foo" => :bar }]
p [{ a: 1 }] == [{ a: 1 }]
p [{ "x" => 1 }] == [{ "x" => 2 }]
p [{ "n" => "m" }] == [{ "n" => "m" }]
mixed = [{ "foo" => :bar, baz: 42 }]
p mixed == [{ "foo" => :bar, baz: 42 }]
