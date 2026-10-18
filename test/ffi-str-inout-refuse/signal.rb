# spinel: not-cruby -- this input must be refused.
module BorrowRefuse
  ffi_func :borrow_refuse, [:str_inout], :void
end
s = String.new("abc")
Signal.trap("INT") { puts 1 }
BorrowRefuse.borrow_refuse(s)
