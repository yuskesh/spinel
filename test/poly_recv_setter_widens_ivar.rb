# #579 (Sam Ruby). `recv.attr = val` where `recv` is statically
# poly (typed sp_RbVal because it came from `case ... when X
# then A.new; when Y then B.new`) lowers to a cls_id-switch
# dispatch in codegen -- each arm does `((sp_C *)_t.v.p)->iv_x = rhs`.
# The analyzer's ivar-type pass only observed setters when recv
# was a single obj_X type; for poly-recv setters every candidate
# class's iv_x stayed at whatever its initialize-time default
# typed it as (e.g. `@data = {}` → sp_StrIntHash *). The per-arm
# assignment then mismatched the wider RHS at the C boundary.
#
# Fix: when scan_writer_calls sees `recv.attr = val` with recv_t
# == "poly", iterate every class that declares the matching
# attr_writer and call update_ivar_type on each. Mirrors the
# codegen dispatch's behavior: every candidate class receives
# the assignment at runtime, so every candidate's ivar type
# must widen against the RHS.

class Base
  attr_accessor :data
  def initialize
    @data = {}
  end
end

class A < Base
end

class B < Base
end

# Force the receiver into the poly shape via a case-expression
# whose branches return different sibling subclasses.
n = 1
receiver = case n
           when 0 then A.new
           when 1 then B.new
           end
merged = { "name" => "foo" }
receiver.data = merged
puts receiver.data["name"]
