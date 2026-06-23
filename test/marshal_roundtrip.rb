# Marshal.dump / Marshal.load round-trip (Phase 1): primitives, Array, Hash.
def rt(x) = Marshal.load(Marshal.dump(x))
p rt(42)
p rt(-7)
p rt(0)
p rt(122)
p rt(123)
p rt(1234567890)
p rt(-987654321)
p rt(3.14)
p rt(-2.5)
p rt(nil)
p rt(true)
p rt(false)
p rt("hello")
p rt("")
p rt(:a_symbol)
p rt([1, 2, 3])
p rt(["a", "b", "c"])
p rt([1, "x", :y, nil, true, [2, 3]])
p rt([])
# Hashes are loaded as a general hash; verify via element access (Hash#inspect
# of a polymorphic hash is a separate, pre-existing gap).
h = rt({"a" => 1, "b" => 2})
puts h["a"]
puts h["b"]
g = rt({x: [1, 2], y: "z"})
puts g[:x].inspect
puts g[:y]
# Bignum.
p rt(123456789012345678901234567890)
p rt(-99999999999999999999999999999999)
p rt(2 ** 200)
p rt([1, 100000000000000000000, "z"])
# Shared references and cycles (CRuby's object-link table).
shared = [1, 2]
sg = rt([shared, shared])
sg[0] << 99
p sg[1]
cyc = [10]
cyc << cyc
dc = rt(cyc)
p dc[0]
p dc.length
p dc[1][1][0]
sh = {n: 1}
eh = rt([sh, sh])
eh[0][:m] = 5
p eh[1][:m]
# Complex and Rational (CRuby `U` user-marshal form). Spinel's Complex is
# float-only, so its components round-trip as Floats.
p rt(Rational(3, 4))
p rt(Rational(-1, 2))
p rt(Rational(5, 1))
p rt(Complex(1.5, -2.5))
p rt([Rational(1, 3), Rational(2, 5)])
rc = rt({r: Rational(7, 8), c: Complex(1.5, -2.5)})
puts rc[:r]
puts rc[:c]
# Plain user objects (CRuby `o` form). Verified via attribute access, since an
# object's #inspect carries a non-deterministic address.
class MPoint
  def initialize(x, y, label)
    @x = x
    @y = y
    @label = label
  end
  attr_reader :x, :y, :label
end
mp = rt(MPoint.new(3, 4, "pt"))
puts mp.x
puts mp.y
puts mp.label
# nil ivar round-trips as nil
class MNil
  def initialize(a); @a = a; end
  attr_reader :a, :b
end
mn = rt(MNil.new(10))
p mn.a
p mn.b
# nested user object + poly ivar holding mixed values
class MInner
  def initialize(v); @v = v; end
  attr_reader :v
end
class MOuter
  def initialize(inner, item); @inner = inner; @item = item; end
  attr_reader :inner, :item
end
mo = rt(MOuter.new(MInner.new(42), "tag"))
puts mo.inner.v
puts mo.item
mo2 = rt(MOuter.new(MInner.new(7), [1, 2]))
puts mo2.inner.v
p mo2.item
# shared object reference round-trips as a shared object
mshared = MInner.new(99)
mpair = rt([mshared, mshared])
p mpair[0].v
p mpair[1].v
