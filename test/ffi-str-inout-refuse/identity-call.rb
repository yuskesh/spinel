# spinel: not-cruby -- a copied ordinary method return is not a borrow route.
module BorrowRefuse
  ffi_func :borrow_refuse, [:str_inout], :void
end
def borrowed_identity(s)
  s
end
s = String.new("abc")
t = borrowed_identity(s)
BorrowRefuse.borrow_refuse(t)
