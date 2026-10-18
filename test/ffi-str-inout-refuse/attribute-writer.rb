# spinel: not-cruby -- a setter may not hide an ordinary return snapshot.
module BorrowRefuse
  ffi_func :borrow_refuse, [:str_inout], :void
end
class BorrowWriter
  attr_reader :data
  attr_writer :data
  def initialize
    @data = String.new("abc")
  end
end
def borrowed_identity(s)
  s
end
b = BorrowWriter.new
s = String.new("abc")
b.data = borrowed_identity(s)
BorrowRefuse.borrow_refuse(b.data)
