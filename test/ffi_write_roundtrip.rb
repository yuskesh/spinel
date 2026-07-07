# ffi_write_u32/i32/ptr mirror ffi_read_*: they store a value at a byte
# offset into a buffer. Round-trip each writer through its matching reader.
# (This uses the spinel-native FFI DSL, so there is no ruby oracle; the
# .expected is authored against the deterministic buffer semantics.)
module Buf
  ffi_buffer :b, 32

  ffi_write_i32 :set_i0, 0
  ffi_read_i32  :get_i0, 0

  ffi_write_u32 :set_u4, 4
  ffi_read_u32  :get_u4, 4

  ffi_write_ptr :set_p8, 8
  ffi_read_ptr  :get_p8, 8
end

Buf.set_i0(Buf.b, -12345)
puts Buf.get_i0(Buf.b)

Buf.set_u4(Buf.b, 4_000_000_000)
puts Buf.get_u4(Buf.b)

# writer returns the value it wrote
puts Buf.set_i0(Buf.b, 77)
puts Buf.get_i0(Buf.b)

# ptr writer: store a foreign pointer and read it back; a NULL reads as nil.
Buf.set_p8(Buf.b, Buf.b)          # store the buffer's own address
puts(Buf.get_p8(Buf.b) != nil)
