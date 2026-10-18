# spinel: not-cruby -- native bindings have a different String ABI.
module BorrowRefuse
  native_func :invalid_native_borrow, [:str_inout], :int, "invalid_native_borrow"
end
puts 0
