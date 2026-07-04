# `recv.attr op= value` where the receiver is only known as a poly value
# (doom: `sector.ceiling_height -= speed` on a sector pulled out of a hash).
class Sector
  attr_accessor :height, :count, :name, :flags
  def initialize(height, count, name, flags)
    @height = height
    @count = count
    @name = name
    @flags = flags
  end
end

# widen: routing the instance through a hash value leaves the local poly-typed
def pick(h)
  h[:sector]
end

# mixed int/float heights and int/nil flags widen those ivars themselves,
# doom-style
h = { sector: Sector.new(64.0, 3, "lift", 1), other: Sector.new(8, 1, "door", nil) }
obj = pick(h)

obj.height -= 1.5
puts obj.height
obj.count += 2
puts obj.count
obj.height *= 2
puts obj.height
obj.count *= 5
puts obj.count
obj.height /= 4.0
puts obj.height
obj.count -= 1
puts obj.count
obj.name += "!"
puts obj.name
# bitwise op on a widened (boxed) slot
obj.flags |= 6
puts obj.flags
# string += with a poly rhs (mixed-value hash lookup)
mix = { s: "?", n: 1 }
obj.name += mix[:s]
puts obj.name
puts h[:sector].height
puts h[:sector].count
puts h[:sector].name
puts h[:sector].flags
