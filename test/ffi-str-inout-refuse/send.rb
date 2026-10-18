# spinel: not-cruby -- dynamic send has no bounded producer graph.
module BorrowRefuse
  ffi_func :borrow_refuse, [:str_inout], :void
end
s = String.new("abc")
s.send(ARGV[0])
BorrowRefuse.borrow_refuse(s)
