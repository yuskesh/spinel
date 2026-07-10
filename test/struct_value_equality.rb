# Struct instances compare by member value (identity was compared before);
# a string/array member compares by content, and eql? follows ==.
S = Struct.new(:a, :b)
p(S.new(1, 2) == S.new(1, 2))
p(S.new(1, 2) == S.new(1, 3))
x = S.new(5, 6)
p(x == S.new(5, 6))
p(x != S.new(5, 6))
p(x.eql?(S.new(5, 6)))
T = Struct.new(:name, :tags)
p(T.new("a", [1, 2]) == T.new("a", [1, 2]))
p(T.new("a", [1, 2]) == T.new("a", [1, 3]))
p(T.new("a", [1]) == T.new("b", [1]))
p(S.new(1, 2).to_a)
