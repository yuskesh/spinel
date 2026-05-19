# Poly-recv user-class dispatch on an ivar previously dragged in
# arms for any class that happened to share the method name, even
# when that class was never assigned to the ivar (never even
# instantiated). Issue #575: an unrelated `run` method on
# Unrelated leaked into the @worker dispatch and forced the
# result temp to widen to sp_RbVal, breaking the downstream
# typed-string consumer.
#
# Fix has two parts:
#   (a) compile_poly_method_call's emit loop consults the existing
#       poly_dispatch_narrow_class_set helper (it was already
#       used by the return-type union but missing from the arm
#       emit).
#   (b) A new analyzer pass forward-propagates each `<C>.new(args)`
#       callsite's arg types into the ivar observations for any
#       `@ivar = pname` write in C#initialize, so the narrow set
#       isn't blocked by the param-union "poly" entry that the
#       writer-scan would have recorded.

class Worker
  def run(item)
    item + ""
  end
end

class Other < Worker
  def run(item)
    item + "!"
  end
end

# Unrelated to Worker. Never instantiated, never assigned to a
# Pool@worker slot. Its `run` has zero formal args and returns
# Integer — pre-fix this leaked into the dispatch and dragged the
# result type to sp_RbVal, tripping the downstream File.write.
class Unrelated
  def run
    1
  end
end

class Pool
  attr_accessor :worker
  def initialize(w); @worker = w; end
  def go(item, path)
    File.write(path, @worker.run(item))
    0
  end
end

Pool.new(Worker.new).go("a", "/tmp/poly_recv_ivar_narrow_a.txt")
Pool.new(Other.new).go("b", "/tmp/poly_recv_ivar_narrow_b.txt")
puts File.read("/tmp/poly_recv_ivar_narrow_a.txt")
puts File.read("/tmp/poly_recv_ivar_narrow_b.txt")
