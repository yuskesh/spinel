# spinel: not-cruby -- unused bodies do not require invented argument types.
module BorrowDead
  ffi_func :borrow_dead, [:str_inout], :void
end
class UnusedBorrow
  def unused(value)
    BorrowDead.borrow_dead(value.no_known_reader)
  end
end
puts "unused body omitted"
