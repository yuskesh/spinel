# CRuby block arity is lenient: a direct instance_exec binds each
# positional param to its arg in order, ignores surplus args, and binds
# a param with no matching arg to nil. Box has a subclass to stay heap.
class Box
  def initialize(v)
    @v = v
  end
end

class BoxPlus < Box
end

b = BoxPlus.new(5)

# surplus args ignored (a=1, the 2 is dropped)
puts b.instance_exec(1, 2) { |a| a + @v }

# missing arg binds nil (a=1, c=nil)
puts b.instance_exec(1) { |a, c| a.to_s + ":" + c.inspect }

# no args, one param -> nil
puts b.instance_exec { |a| a.inspect }

# args but no params -> args ignored, body still runs
puts b.instance_exec(7, 8) { @v * 2 }
