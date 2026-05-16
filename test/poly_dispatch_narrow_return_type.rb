# #549. Followup to #531. The cls_id-switch dispatch's RESULT
# temp widened to sp_RbVal even when every reachable arm
# returned the same scalar. The reachable set is computed by
# the same arm-suppression logic the codegen emit loop applies:
# arms whose param types are incompatible with the dispatch
# site's arg types are pruned, and only arms in the observed
# obj-class set survive. With both filters applied, the return
# types of surviving arms are unified; if they agree on a
# scalar (e.g. all `const char *`), the result temp uses that
# scalar instead of sp_RbVal, and the boxed arm rhs (`_t =
# sp_box_str(...)`) collapses to a direct assignment.
#
# Shape: M::Server#run takes `port:int` and returns int; the
# Worker family takes `item:string` and returns string. The
# dispatch site `@w.run(item)` supplies a string, so M::Server's
# arm is param-incompat and pruned. The Worker arms return
# string, so the dispatch produces `const char *` and the
# scalar-only consumer `s + "."` below compiles correctly.
# Pre-fix, the dispatch result was sp_RbVal and `s + "."`
# routed through sp_poly_concat -- still ran but the
# concatenated form leaked the boxing.
#
# Discriminator: `Holder#use` returns `s + "."`. If the
# dispatch result is sp_RbVal, `s + "."` lowers through
# sp_poly_concat, which formats the box and produces e.g.
# `x!.` only because sp_box_str / sp_poly_concat happen to
# preserve the underlying string. The shape is correct but
# the path is wrong; once the dispatch is narrowed to
# const char *, `s + "."` lowers to sp_str_concat which is
# what we want.

module M
  class WorkerA
    def run(item); item + "!"; end
  end
  class WorkerB
    def run(item); item + "?"; end
  end
  class Holder
    def initialize(w); @w = w; end
    def use(item)
      s = @w.run(item)
      s + "."
    end
  end
  class Server
    def run(port); port + 1; end
  end
end

puts M::Holder.new(M::WorkerA.new).use("x")
puts M::Holder.new(M::WorkerB.new).use("y")
puts M::Server.new.run(80)
