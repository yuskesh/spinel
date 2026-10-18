# spinel: not-cruby -- pack(buffer:) may return an existing String.
module BorrowRefuse
  ffi_func :borrow_refuse, [:str_inout], :void
end
s = String.new("abc")
t = [90].pack("C", buffer: s)
BorrowRefuse.borrow_refuse(t)
