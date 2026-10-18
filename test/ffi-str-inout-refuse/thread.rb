# spinel: not-cruby -- this input must be refused.
module BorrowRefuse
  ffi_func :borrow_refuse, [:str_inout], :void
end
s = String.new("abc")
Thread.new { puts 1 }.join
BorrowRefuse.borrow_refuse(s)
