# A Hash reached through a poly value must dispatch `fetch` to its storage the
# same way `[]` does. The PR added poly `[]` arms but left `fetch` arm-less, so
# `fetch(present_key)` on a poly-valued hash fell through to the seeded default
# (or dropped its value as nil) even when the key existed. Cover fetch-present,
# fetch-with-default (absent), fetch-raising KeyError (absent), and a poly key.
class Sel
  def [](i); i * 10; end          # a user `[]` forces per-class poly dispatch
  def fetch(i); i * 100; end      # a user `fetch` too
end
class Holder
  def initialize(real)
    h = {}
    h["a"] = 1
    h[2]   = "b"                  # heterogeneous keys -> PolyPolyHash
    @map = real ? h : "none"      # poly-valued ivar (widens the Hash to poly)
  end
  def fetch_present(name); @map.fetch(name); end          # string key, present
  def fetch_default(name); @map.fetch(name, -1); end       # string key, absent
  def poly_key(name); @map.fetch(name.length > 9 ? nil : name); end  # poly key
end
h = Holder.new(true)
puts "present: #{h.fetch_present("a")}"
puts "default: #{h.fetch_default("zz")}"
puts "poly key: #{h.poly_key("a")}"
begin
  h.fetch_present("nope")
  puts "no raise (BUG)"
rescue KeyError
  puts "KeyError"
end
# the user-class arms still win when the poly value is really a Sel
puts "user fetch: #{Sel.new.fetch(3)}"
