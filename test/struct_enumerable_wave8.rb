# Struct: Enumerable via synthesized #each/#each_pair, size/length,
# deconstruct_keys, [] with string keys, multi-key dig, member []=,
# ctor arity ArgumentError, class-level members, class-in-variable new.
Pt141 = Struct.new(:x, :y)
p(Pt141.new(3, 4).map { |v| v * 2 })
p(Pt141.new(3, 4).each { |v| p v })
p(Pt141.new(3, 4).select { |v| v > 3 })
p(Pt141.new(3, 4).sum)
p(Pt141.new(3, 4).min)
p(Pt141.new(3, 4).include?(4))
Pt142 = Struct.new(:x, :y)
p(Pt142.new(10, 20)["x"])
p(Pt142.new(10, 20)[:y])
p(Pt142.new(10, 20)[0])
In142 = Struct.new(:x)
Ou142 = Struct.new(:pt)
n = Ou142.new(In142.new(1))
p(n.dig(:pt, :x))
Pt145 = Struct.new(:x, :y)
pt = Pt145.new(1, 2)
pt[0] = 99
pt[:y] = 88
p pt
Pt143 = Struct.new(:x, :y)
r = (begin; Pt143.new(1, 2, 3); "no error"; rescue ArgumentError; "argerror"; end); p r
Kk144 = Struct.new(:a, :b)
p(Kk144.members)
k = Kk144
inst = k.new(1, 2)
p inst.a
Pk = Struct.new(:a, :b)
Pk.new(1, 2).each_pair { |k, v| p [k, v] }
p Pk.new(1, 2).size
p Pk.new(1, 2).length
p Pk.new(1, 2).deconstruct_keys([:a])
p Pk.new(1, 2).deconstruct_keys(nil)
