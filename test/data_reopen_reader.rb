# A Data.define (or Struct.new) generated reader must resolve inside a method
# defined on the REOPENED class.
#
# `D = Data.define(:x)` is a ConstantWriteNode; the following `class D ... end`
# is a ClassNode. walk_scope runs before struct registration and, not knowing D
# is a Data class, pre-creates a memberless class D. Struct registration then
# saw D already existed and skipped it, so `def double = x * 2` could not
# resolve the generated reader `x` and rejected the arithmetic. Register the
# members onto the existing class instead of skipping.

D = Data.define(:x)
class D
  def double = x * 2
end
p D.new(3).double                  #=> 6

# Multiple members, several methods.
P = Data.define(:x, :y)
class P
  def sum = x + y
  def to_s = "(#{x}, #{y})"
end
p P.new(3, 4).sum                  #=> 7
puts P.new(1, 2).to_s              #=> (1, 2)

# Data#with produces a new instance whose reader still resolves.
p D.new(5).with(x: 10).double      #=> 20

# Struct reopen works the same way.
S = Struct.new(:a)
class S
  def twice = a * 2
end
p S.new(7).twice                   #=> 14

# The block form (no reopen) still works.
E = Data.define(:v) do
  def neg = -v
end
p E.new(9).neg                     #=> -9

puts "done"
