# spinel: not-cruby -- a value-position mutation is not an admitted borrow route.
module BorrowRefuse
  ffi_func :borrow_refuse, [:str_inout], :void
end
s = String.new("abc")
BorrowRefuse.borrow_refuse(s << "d")
