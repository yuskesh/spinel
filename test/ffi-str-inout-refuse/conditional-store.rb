# spinel: not-cruby -- this slice does not prove conditional field stores.
module BorrowRefuse
  ffi_func :borrow_refuse, [:str_inout], :void
end
class ConditionalBorrow
  attr_reader :data
  def initialize
    @data = String.new("abc")
  end
  def prepare
    @data ||= String.new("xyz")
  end
end
b = ConditionalBorrow.new
b.prepare
BorrowRefuse.borrow_refuse(b.data)
