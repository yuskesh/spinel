# Hash store/default reader+writer/Hash.new(default) literal folds,
# Array concat value form, fetch with default or block, repeated_permutation.
p({ a: 1 }.store(:b, 2))
h = { "x" => 1 }
p(h.store("y", 5))
p h
g = { 1 => "a" }
g.store(2, "b")
p g
p(Hash.new(7).default)
p(({}.default = 9))
p(Hash.new(0)[:missing])
p(Hash.new("d")["nope"])
h = Hash.new(42)
p h.default
h.default = 5
p h.default
p h[:zzz]
a = [1, 2]
a.concat([3, 4])
p a
b = [1, 2]
r = b.concat([3])
p r
p([10, 20, 30].fetch(5, "default"))
p([10, 20, 30].fetch(1, "default"))
p([10, 20, 30].fetch(5) { |i| "missing #{i}" })
p([10, 20, 30].fetch(-1))
p([10, 20, 30].fetch(1) { |i| i * 100 })
p([10, 20, 30].fetch(9) { |i| i * 100 })
mixed = [1, "x"]
p(mixed.fetch(7, :none))
p([1, 2].concat([3, 4]))
b = [1, 2]
r = b.concat([3])
p r
p b
s = %w[a b]
p(s.concat(%w[c], %w[d e]))
sc = [1, 2]
p(sc.concat(sc))
p([1, 2].repeated_permutation(2).to_a)
p(%w[a b].repeated_permutation(2).to_a)
p([1, 2, 3].repeated_permutation(0).to_a)
