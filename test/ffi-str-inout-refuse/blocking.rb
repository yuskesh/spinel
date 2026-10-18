# spinel: not-cruby -- this input must be refused.
module BorrowRefuse
  ffi_func :borrow_refuse, [:str_inout], :void, blocking: true
end
s = String.new("abc")
BorrowRefuse.borrow_refuse(s)
