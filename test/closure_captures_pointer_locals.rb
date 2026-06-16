# A closure that escapes (stored in an ivar -> real proc) may now capture
# pointer-typed locals -- string, array, heap object -- not just ints. Each is
# laundered through the int cell and the cell's GC scan marks the referent, so
# it survives after the defining method returns and across a collection.
class Box
  def initialize(v)
    @v = v
  end
  def v
    @v
  end
end
class BoxPlus < Box   # subclass keeps Box heap (not a by-value type)
end

class Holder
  def store(&blk)
    @blk = blk
  end
  def fire
    @blk.call
  end
end

def capture_string
  s = "hello-" + 7.to_s
  h = Holder.new
  h.store { s + "!" }
  h
end

def capture_array
  a = [10, 20, 30]
  h = Holder.new
  h.store { a.length }
  h
end

def capture_object
  b = BoxPlus.new(42)
  h = Holder.new
  h.store { b.v }
  h
end

hs = capture_string
ha = capture_array
ho = capture_object

# churn the heap so a GC runs after the defining methods returned; the captured
# referents must survive only via each proc's cap-cell scan.
acc = 0
i = 0
while i < 50000
  junk = "g-" + i.to_s
  acc = acc + junk.length
  i = i + 1
end

puts hs.fire     # hello-7!
puts ha.fire     # 3
puts ho.fire     # 42
puts acc > 0     # true
