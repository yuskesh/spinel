class C; end
o = C.new
puts o.frozen?
o.freeze
puts o.frozen?
S = Struct.new(:a)
x = S.new(1)
puts x.frozen?
x.freeze
puts x.frozen?
y = S.new(2)
puts y.frozen?
