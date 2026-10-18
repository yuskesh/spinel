# spinel: not-cruby -- explicit synchronous mutable String borrowing.
module Borrow
  ffi_lib "c"
  ffi_source <<~'C'
    static uintptr_t borrow_previous;
    int borrow_write(char *p, int n) {
      int same = (uintptr_t)p == borrow_previous;
      borrow_previous = (uintptr_t)p;
      if (n) p[0] = 'Z';
      if (n > 1) p[1] = 0;
      return same;
    }
  C
  ffi_func :borrow_write, [:str_inout, :int], :int
end
class Box
  attr_reader :data
  def initialize
    @data = String.new("abc")
  end
  def change(s)
    Borrow.borrow_write(s, s.bytesize)
  end
end
b = Box.new
s = b.data
copy = s.dup
puts b.change(s)
puts b.change(s)
puts s.getbyte(0)
puts s.getbyte(1)
puts s.bytesize
puts b.data.getbyte(0)
puts copy.getbyte(0)
