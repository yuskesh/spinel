# spinel: not-cruby -- this input must be refused.
module CallbackDecl
  ffi_callback :unused_callback, [], :void
end
module BorrowRefuse
  ffi_func :borrow_refuse, [:str_inout], :void
end
s = String.new("abc")
BorrowRefuse.borrow_refuse(s)
