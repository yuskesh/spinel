module M
  ffi_lib "c"
  ffi_func :abs, [:int], :int
end
class P
  def initialize
    @mult = 2
    @base = 5
  end
  def backoff_for(n)
    d = @base
    i = 0
    while i < n
      d = d * @mult
      i += 1
    end
    d
  end
end
p = P.new
b = p.backoff_for(3)   # 5*2*2*2 = 40
puts M.abs(b)
