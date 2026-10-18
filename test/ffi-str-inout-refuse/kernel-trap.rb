# spinel: not-cruby -- Kernel.trap is a Ruby entry point as Signal.trap is.
module BorrowRefuse
  ffi_func :borrow_refuse, [:str_inout], :void
end
s = String.new("abc")
Kernel.trap("INT") { puts 1 }
BorrowRefuse.borrow_refuse(s)
