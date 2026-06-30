# p / puts / print / interpolation / #inspect / #to_s of Struct and Data
# instances render like CRuby. A user-defined #inspect or #to_s overrides the
# generated one. Regular objects with a user #inspect also print via p now.
P = Data.define(:a, :b)
x = P.new(a: 1, b: 2)
p x
puts x
puts x.inspect
puts x.to_s
puts "val=#{x}"

S = Struct.new(:m, :n)
y = S.new(3, "hi")
p y
puts "s=#{y}"

# nested struct member inspects recursively
Q = Data.define(:inner)
p Q.new(inner: P.new(a: 9, b: 8))


# user-defined inspect/to_s win over the generated ones
class Pt
  def initialize(v); @v = v; end
  def inspect; "Pt(#{@v})"; end
  def to_s; "pt:#{@v}"; end
end
p Pt.new(7)
puts Pt.new(8)
