# Hash: blockless any?/all?/none?, Hash[kwargs], deconstruct_keys,
# multi-arg merge with a conflict block, min_by(n)/max_by(n) (hash receivers),
# compact!, and &hash as a block argument.
p({ a: 1 }.any?)
p({}.any?)
p({ a: 1 }.all?)
p({}.all?)
p({ a: 1 }.none?)
p({}.none?)
p Hash[a: 1, b: 2]
h0 = { name: "x", age: 3 }
p h0.deconstruct_keys([:name])
p h0.deconstruct_keys(nil)

h1 = { a: 1 }
p h1.merge({ a: 10 }, { a: 20 }) { |k, o, n| o + n }
p h1.merge({ b: 2 }, { c: 3 })
g1 = { "x" => 1 }
p g1.merge({ "x" => 5 }) { |k, o, n| o * n }

h2 = { a: 1, b: 2 }
p [:a, :b].map(&h2)
g2 = { "x" => 10 }
p %w[x y].map(&g2)

h3 = { a: 3, b: 1, c: 2 }
p h3.min_by(2) { |_k, v| v }
p h3.max_by(2) { |_k, v| v }
p [5, 1, 4].min_by(2) { |x| x }
p [5, 1, 4].max_by(2) { |x| x }

h4 = { a: 1, b: nil, c: 3 }
p h4.compact!
p h4
g4 = { a: 1 }
p g4.compact!
p g4
