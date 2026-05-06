# `Klass.method(:cls_meth)` returns a Method object that wraps a
# class method (`def self.foo`).  Spinel previously only handled
# instance-receiver `obj.method(:bar)`, so the class-receiver form
# fell through to the unresolved-call fallback (emitting `0`) and
# the captured "Method" was actually an int.  Subsequent
# `.call(args)` then warned `cannot resolve call to 'call' on int`
# and returned 0.
#
# Repro: capture two class methods on the same class, then call
# both via `.call`.  The wrapping must:
#   - infer the result as `obj_Method` (not int),
#   - mark the underlying cls method as live (so the adapter
#     trampoline's `sp_<Klass>_cls_<m>` reference resolves at
#     link time — without the live-mark the body is DCE'd and the
#     C link fails),
#   - bind through an adapter that absorbs the dispatch ABI's
#     `void *self` slot (class methods have no `self *` param).

class CPU
  def self.poke_nop(addr, data)
    addr + data
  end
  def self.poke_log(addr, data)
    addr * 2 + data
  end
end

m1 = CPU.method(:poke_nop)
m2 = CPU.method(:poke_log)
puts m1.call(10, 5)
puts m2.call(10, 5)
