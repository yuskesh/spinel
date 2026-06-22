# An unwritten instance variable reads as nil, so a conditional first write
# (`||=`) sees nil and assigns rather than seeing a typed zero.
class Counter
  def bump
    @n ||= 10
    @n += 1
    @n
  end
end
p Counter.new.bump      # 11  (was 1: @n had started at 0, so `0 ||= 10` kept 0)

# `.nil?` and `|| default` on an unwritten int ivar.
class Box
  def empty?
    @v.nil?
  end

  def value
    @v || 42
  end
end
b = Box.new
p b.empty?              # true
p b.value               # 42

# An unconditionally initialized int ivar is unaffected (stays its real value).
class Init
  def initialize
    @k = 0
  end

  def k
    @k
  end
end
p Init.new.k            # 0
