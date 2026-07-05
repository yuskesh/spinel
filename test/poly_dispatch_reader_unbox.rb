# A length-like poly method dispatch fixes its result slot to mrb_int, but an
# attr-reader arm can name a poly-typed ivar (a struct member fed from a poly
# source): the read must unbox into the scalar slot, mirroring the method-arm
# coercion (doom: `@file.read(entry.size)` on a WAD directory entry whose
# fields come from String#unpack1).
E = Struct.new(:offset, :size, :name)

class Box
  def initialize(n)
    @n = n
  end

  def bump!
    @n += 1
  end

  def size
    @n
  end
end

def lookup(arr, n)
  arr.find { |e| e.name == n }
end

meta = [4, "x"]
arr = [E.new(0, meta[0], "A"), "sentinel"]
entry = lookup(arr, "A")
if entry
  n = entry.size
  p n + 1
end

b = Box.new(3)
b.bump!
p b.size
