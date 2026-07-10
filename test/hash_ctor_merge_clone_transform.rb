# The Hash[] constructor, multi-argument merge/merge!, Array#clone's frozen
# flag, inline Hash.new(default) arguments, Array#slice!(i)/(range),
# transform_values!/transform_keys! and transform_keys(mapping), and
# multi-candidate start_with?/end_with?.
p Hash[[[:a, 1], [:b, 2]]]
p Hash[:a, 1, :b, 2]
p({ a: 1 }.merge({ b: 2 }, { c: 3 }))
h = { a: 1 }
h.merge!({ b: 2 }, { c: 3 })
p h
a = [1, 2, 3].freeze
p a.clone.frozen?
p a.dup.frozen?
p a.clone(freeze: false).frozen?
p [1, 2, 2, 3].each_with_object(Hash.new(0)) { |x, acc| acc[x] += 1 }
b = [1, 2, 3, 4]
p b.slice!(1)
p b.slice!(1..2)
p b
hv = { a: 1, b: 2 }
hv.transform_values! { |v| v * 10 }
p hv
hk = { a: 1, b: 2 }
hk.transform_keys! { |k| k.to_s }
p hk
p({ a: 1, b: 2 }.transform_keys(a: :x))
p "hello".start_with?("x", "he")
p "test.rb".end_with?(".py", ".rb")
p "abc".start_with?("z", "q")
