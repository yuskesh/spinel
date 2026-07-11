# Comparable <,>,<=,>=,between?,clamp on a class whose `<=>` returns nil
# for an incomparable (mixed-type) operand. The operators route through the
# checked boxed comparator even for a VALUE-TYPE class (the direct inline
# emit compared an sp_RbVal against 0: invalid C), the cmp-dispatch hook
# gets an arm for a scalar-typed `<=>` param, and the ArgumentError message
# describes a special-constant operand by its inspect ("with 3"), not its
# class name ("with Integer"), like CRuby's rb_cmperr.
class VerN
  include Comparable
  attr_reader :n
  def initialize(n); @n = n; end
  def <=>(o); return nil unless o.is_a?(VerN); n <=> o.n; end
end
a = VerN.new(5)
p(a < VerN.new(9))
p(a > VerN.new(9))
p(a <= VerN.new(5))
p(a >= VerN.new(5))
p(a.between?(VerN.new(1), VerN.new(9)))
p(a.clamp(VerN.new(1), VerN.new(3)).n)
begin; p(a < 3); rescue ArgumentError => e; puts "AE: #{e.message}"; end
begin; p(a.between?(1, 9)); rescue ArgumentError => e; puts "AE: #{e.message}"; end
begin; p(a.clamp(1, 9)); rescue ArgumentError => e; puts "AE: #{e.message}"; end

# heap-class twin (an extra ivar of another type defeats value-type detection)
class HeapV
  include Comparable
  def initialize(n); @n = n; @tag = "h#{n}"; end
  attr_reader :n, :tag
  def <=>(o); return nil unless o.is_a?(HeapV); n <=> o.n; end
end
h = HeapV.new(5)
p(h < HeapV.new(9))
begin; p(h < :sym); rescue ArgumentError => e; puts "AE: #{e.message}"; end
begin; p(h > nil); rescue ArgumentError => e; puts "AE: #{e.message}"; end
