# spinel: not-cruby -- eval has no bounded producer graph.
module BorrowRefuse
  ffi_func :borrow_refuse, [:str_inout], :void
end
s = String.new("abc")
eval("1")
BorrowRefuse.borrow_refuse(s)
