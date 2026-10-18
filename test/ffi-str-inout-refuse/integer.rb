# spinel: not-cruby -- this input must be refused.
module BorrowRefuse
  ffi_func :borrow_refuse, [:str_inout], :void
end
BorrowRefuse.borrow_refuse(42)
