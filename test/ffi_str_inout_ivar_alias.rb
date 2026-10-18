# spinel: not-cruby -- a local read of a field must retain its original handle.
module BorrowIvar
  ffi_source <<~'C'
    void borrow_ivar_write(char *p) { p[0] = 'Z'; }
  C
  ffi_func :borrow_ivar_write, [:str_inout], :void
end
class BorrowIvarBox
  def initialize
    @data = String.new("abc")
  end
  def run
    s = @data
    BorrowIvar.borrow_ivar_write(s)
    puts s.getbyte(0)
    puts @data.getbyte(0)
  end
end
BorrowIvarBox.new.run
