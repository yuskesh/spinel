# spinel: not-cruby -- boxed parameters do not establish String identity.
module BorrowRefuse
  ffi_func :borrow_refuse, [:str_inout], :void
end
def borrow_boxed(value)
  BorrowRefuse.borrow_refuse(value)
end
borrow_boxed(1)
borrow_boxed(String.new("abc"))
