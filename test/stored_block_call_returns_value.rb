# A block stored into an ivar (`@blk = blk`) is type-erased into a generic
# sp_Proc*; a later `@blk.call` reads the boxed poly return slot
# (_sp_proc_poly_ret). The block must therefore use the poly return ABI even
# when its body yields a scalar -- otherwise the proc returns the int directly
# and the generic call site reads a stale nil (printed 0).
class Holder
  def store(&blk)
    @blk = blk
  end
  def fire
    @blk.call
  end
end

h = Holder.new
h.store { 42 }
puts h.fire           # 42
h.store { 7 + 8 }
puts h.fire           # 15
