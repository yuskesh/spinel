# spinel: not-cruby -- unproved borrow identity route.
module BorrowRefuse
  ffi_func :borrow_refuse, [:str_inout], :void
end
s = String.new("abc")
l = -> { s = String.new("def") }
l.call
BorrowRefuse.borrow_refuse(s)
