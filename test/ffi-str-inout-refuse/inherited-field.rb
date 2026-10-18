# spinel: not-cruby -- inherited field producers need a separate proof.
module BorrowRefuse
  ffi_func :borrow_refuse, [:str_inout], :void
end
class BorrowParent
  attr_reader :data
  def initialize
    @data = String.new("abc")
  end
end
class BorrowChild < BorrowParent
end
b = BorrowChild.new
BorrowRefuse.borrow_refuse(b.data)
