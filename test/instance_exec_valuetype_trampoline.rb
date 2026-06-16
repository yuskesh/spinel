# A value-type receiver (small immutable class promoted to a by-value struct)
# reached through an instance_exec *trampoline* method. The trampoline splice
# must bind the rebound self by value and dereference its ivars with `.`, not
# treat it as a pointer -- the receiver here is an sp_Pt struct, not sp_Pt*.
# The trampoline body also forwards the receiver's own ivars as args, which are
# read through the by-value self.

class Pt
  def initialize(x, y)
    @x = x
    @y = y
  end

  # trampoline forwarding two ivars to the block
  def combine(&b)
    instance_exec(@x, @y, &b)
  end

  # trampoline forwarding an ivar plus a forwarded local
  def scale(k, &b)
    instance_exec(@x, k, &b)
  end
end

p1 = Pt.new(3, 4)
p2 = Pt.new(10, 20)

# expression position: the block value flows into puts
puts(p1.combine { |a, b| a + b })     # 7
puts(p2.combine { |a, b| a * b })     # 200
puts(p1.scale(5) { |x, k| x * k })    # 15

# statement position
p2.combine { |a, b| puts(a + b) }     # 30

puts "done"
