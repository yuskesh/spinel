# spinel: not-cruby -- this input must be refused.
module BorrowRefuse
  ffi_func :borrow_refuse, [], :str_inout
end
BorrowRefuse.borrow_refuse
