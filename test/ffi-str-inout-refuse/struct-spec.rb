# spinel: not-cruby -- no stored raw mutable borrow in a struct field.
module BorrowRefuse
  ffi_struct :InvalidBorrow, [[:data, :str_inout]]
end
puts 0
