# spinel: not-cruby -- unproved borrow identity route.
module BorrowRefuse
  ffi_func :borrow_refuse, [:str_inout], :void
end
a = [String.new("abc")]
BorrowRefuse.borrow_refuse(a[0])
