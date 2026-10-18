# spinel: not-cruby -- native writes must invalidate cached String observations.
module BorrowCache
  ffi_source <<~'C'
    void borrow_cache_write(char *p) { p[0] = (char)0xc3; p[1] = (char)0xa9; }
  C
  ffi_func :borrow_cache_write, [:str_inout], :void
end
s = String.new("abc")
copy = s.dup
puts s.length
puts s.ascii_only?
before = s.hash
BorrowCache.borrow_cache_write(s)
puts s.bytesize
puts s.length
puts s.ascii_only?
puts s.hash == "\u00e9c".hash
puts s.hash == before
puts copy.getbyte(0)
