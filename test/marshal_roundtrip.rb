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
