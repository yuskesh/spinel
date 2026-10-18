# spinel: not-cruby -- explicit mutable String FFI borrow.
module BorrowEdges
  ffi_source <<~'C'
    static int borrow_edge_calls;
    int borrow_edge_count(void) { return borrow_edge_calls; }
    int borrow_edge_write(char *a, int n, char *b, int m) {
      borrow_edge_calls++;
      if (n) a[0] = 'X';
      if (m) b[m-1] = 'Y';
      return n + m;
    }
  C
  ffi_func :borrow_edge_count, [], :int
  ffi_func :borrow_edge_write, [:str_inout, :int, :str_inout, :int], :int
end
s = String.new("abc")
alias_s = s
other = s.dup
empty = String.new
puts BorrowEdges.borrow_edge_write(s, s.bytesize, empty, empty.bytesize)
puts alias_s.getbyte(0)
puts other.getbyte(0)
puts empty.bytesize
puts BorrowEdges.borrow_edge_write(s, 3, s, 3)
puts s.getbyte(2)
s.freeze
begin
  BorrowEdges.borrow_edge_write(s, 3, empty, 0)
  puts "C was called"
rescue FrozenError
  puts "frozen refused"
end
puts BorrowEdges.borrow_edge_count
