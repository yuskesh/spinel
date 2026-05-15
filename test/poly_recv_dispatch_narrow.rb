# #513. When `obj.run(arg)` virtual-dispatched through an
# unpinned receiver slot, the generated cls_id switch
# enumerated EVERY class that defines `run`, not just classes
# that can reach the receiver. Unrelated classes that share
# the method name got pulled into the switch and their
# parameters were widened to sp_RbVal to accept the dispatch
# site's arg.
#
# Fix: when scan_new_calls' poly-recv widening fires for
# `recv.mname(...)` and the recv is an ivar read, compute the
# observed-class set for that ivar via observed_class_ids_for_recv.
# When the recorded observation is just "poly" (because the rhs
# of `@x = w` was a poly-typed param), walk the class's call
# sites for the enclosing method and aggregate the concrete
# obj types observed at the matching arg position. Restrict
# the per-class param-widening loop to that set.
#
# The codegen-side cls_id-switch then naturally drops the
# unrelated arms via the existing arm_incompat check: classes
# whose `run` param stayed at the original concrete type can't
# accept the dispatch site's (different concrete) arg, so the
# arm is suppressed.

class WorkerA
  def run(item)
    item + "!"
  end
end

class WorkerB
  def run(item)
    item + "?"
  end
end

class Holder
  def initialize(w)
    @w = w
  end
  def use(item)
    @w.run(item)
  end
end

class Server
  def run(port)
    port + 1
  end
end

# The fix: Server#run's `port` stays mrb_int (not widened to
# sp_RbVal by the @w.run(item) dispatch), so the int arithmetic
# `port + 1` compiles cleanly and the direct call
# `Server.new.run(80)` doesn't have a poly-cast/unbox layer.
puts Holder.new(WorkerA.new).use("x")
puts Holder.new(WorkerB.new).use("y")
puts Server.new.run(80)
