# The instance under construction must survive its own initialize body.
#
# When `initialize` contains a `yield`, the compiler inlines the constructor
# into a statement expression: it calls `sp_X_new()`, splices the initialize
# body in, and yields the instance pointer as the value of the expression --
# and only the CALLER roots that result. The inlined body allocates freely
# (its locals, the ivar containers it builds, every method it calls), so a
# collection inside the body reclaims the half-built instance, which lives
# only in a plain C temporary the collector cannot see. The caller's root
# then registers a dangling slot.
#
# The loop and the allocating body are what make this reproduce at the
# DEFAULT collection threshold rather than only under SPINEL_GC_STRESS: the
# body has to allocate enough on its own to cross the threshold while the
# instance is still unrooted, and the drifting `pad` sweeps the phase so the
# collection lands inside the window rather than beside it.
class Bag
  def initialize(n)
    @items = []
    i = 0
    while i < n
      @items << [i, i * 2]
      i += 1
    end
    yield self
  end

  def size
    @items.size
  end
end

lost = 0
i = 0
while i < 60
  pad = Array.new(i % 11) { |z| [z] }
  n = 500 + i * 7
  b = Bag.new(n) { |x| x }
  lost += 1 if b.size != n
  i += 1
end

p lost > 0
