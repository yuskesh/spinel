# spinel: not-cruby -- this contract is not a callback parameter spec.
module BorrowRefuse
  ffi_callback :invalid_borrow, [:str_inout], :void
end
puts 0
