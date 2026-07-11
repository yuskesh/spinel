# Hash methods flowing through user-method returns: compact, flatten,
# each_with_index, each_with_object with an empty {} seed, Hash[pairs]
# with a non-literal argument, and chunk grouping consecutive pairs.
def fc(h)
  g = h.compact
  g
end
def fl(h); h.flatten; end
def fw(h)
  r = []
  h.each_with_index { |pair, i| r << [i, pair[0]] }
  r
end
def fo(h)
  h.each_with_object({}) { |(k, v), acc| acc[v] = v * v }
end
def fb(a); Hash[a]; end
def fk(h)
  h.chunk { |k, v| v.even? }.to_a
end
def fs(h)
  h.chunk { |k, v| v[0] }.to_a
end
def fn(h)
  h.chunk { |k, v| v.zero? ? nil : :pos }.to_a
end
def fa(h)
  h.chunk { |k, v| :_alone }.to_a
end

p fc({a: 1, b: nil, c: 3})     # {a: 1, c: 3}
p fc({x: nil})                 # {}
p fl({a: 1, b: 2})             # [:a, 1, :b, 2]
p fw({a: 1, b: 2})             # [[0, :a], [1, :b]]
p fo({a: 1, b: 2})             # {1 => 1, 2 => 4}
p fb([[:a, 1], [:b, 2]])       # {a: 1, b: 2}
p fk({a: 1, b: 2, c: 4})       # [[false, [[:a, 1]]], [true, [[:b, 2], [:c, 4]]]]
p fk({a: 2, b: 4, c: 6})       # one all-true run
p fs({"x" => "apple", "y" => "avocado", "z" => "berry"})  # grouped by first letter
p fn({a: 1, b: 0, c: 3})       # nil key drops the pair and splits the run
p fa({a: 1, b: 1})             # :_alone chunks each pair separately
