# Float() hex-float strings, Symbol#encoding, is_a?(Object) on typed
# user objects, and boolean & | ^ with arbitrary (truthy) operands.
p(Float("0x1p4"))
p(Float("0x1.8p1"))
p(:a.encoding)
p("x".encoding)
class K118; end
o = K118.new
p o.is_a?(Object)
p o.is_a?(BasicObject)
p o.kind_of?(Object)
p(true & 5)
p(false | "x")
p(true ^ nil)
p(Float("0xa"))
r = (Float("0x1p") rescue "bad"); p r
r2 = (Float("0xp4") rescue "bad"); p r2
