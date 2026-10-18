# spinel: not-cruby -- later argument preparation may resize and collect.
module BorrowOrder
  ffi_source <<~'C'
    int borrow_order_write(char *p, int n) { if (p[0] != 'a') return -1; p[n-1] = 'Z'; return n; }
  C
  ffi_func :borrow_order_write, [:str_inout, :int], :int
end
class BorrowState
  def data
    puts "read"
    @data
  end
  def initialize
    @data = String.new("a")
  end
  def prepare
    puts "prepare"
    @data << ("b" * 100000)
    @data = String.new("c" * 200001)
    GC.start
    100001
  end
end
b = BorrowState.new
puts BorrowOrder.borrow_order_write(b.data, b.prepare)
puts b.data.getbyte(100000)
puts b.data.bytesize
