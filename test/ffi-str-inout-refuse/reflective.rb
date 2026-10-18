# spinel: not-cruby -- reflection is not a proven borrowed holder route.
module BorrowRefuse
  ffi_func :borrow_refuse, [:str_inout], :void
end
class ReflectBorrow
  def initialize
    @data = String.new("abc")
  end
end
b = ReflectBorrow.new
b.instance_variable_set(:@data, String.new("xyz"))
s = String.new("abc")
BorrowRefuse.borrow_refuse(s)
