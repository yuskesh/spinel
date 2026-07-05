# A string-keyed Hash held in a poly value and indexed by `[]` with a poly key
# (a key whose static type is not narrowed to String) must reach the hash
# storage even when a user class defines `[]` (routing `[]` through the
# per-class poly dispatch). Without hash arms there the lookup returned nil --
# doom's `@textures[anim_texture(name)]` (a StrPolyHash arriving as a poly
# value, poly key) resolved to nil so walls drew untextured.
class Sel
  def [](i); i * 10; end   # a user `[]` forces per-class poly dispatch
end
class Holder
  def initialize(real)
    h = {}
    h["a"] = [1, 1]   # poly (array) values -> StrPolyHash
    h["b"] = [2, 2]
    @map = real ? h : "none"        # poly-valued ivar
  end
  def kk(n); n.length > 3 ? nil : n; end   # returns String|nil -> a poly key
  def get(name); @map[kk(name)]; end
end
h = Holder.new(true)
p h.get("a")
p h.get("b")
p h.get("zz")
puts Sel.new[4]
