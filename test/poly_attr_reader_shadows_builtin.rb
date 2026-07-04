# An attr_reader/attr_accessor whose name collides with a builtin poly
# zero-arg helper (value/resume/join -> Thread#value, sum/min/max/first/
# last/sample -> array reductions) must shadow the helper exactly like an
# explicit `def` does.
#
# The shortcut in emit_call only counted comp_method_in_chain when deciding
# "no user class defines this name", so an accessor-declared reader was
# invisible: `node.value` on a poly element emitted sp_poly_fiber_value(...)
# (an sp_RbVal), and typed arithmetic on the result failed C compilation with
#   invalid operands to binary expression ('sp_RbVal' and 'sp_RbVal')
#
# Reduced from the BART engine, where @roots (a poly array of Nodes, ivar
# arrays never narrow to obj arrays) made Tree.leaf_stats' `node.value *
# node.value` the build blocker.

class Node
  attr_accessor :value, :first
  attr_reader :max
  def initialize
    @value = 1.5
    @first = 7
    @max = "peak"
  end
end

class Forest
  def initialize
    @roots = []          # ivar array of user objects: stays poly_array
    @roots << Node.new
  end

  def go
    @roots[0].value * @roots[0].value   # Float reader named like Thread#value
  end

  def head
    n = @roots[0]
    n.first + n.first                   # Int reader named like Array#first
  end

  def peak
    @roots[0].max                       # String reader named like Array#max
  end
end

f = Forest.new
puts f.go        # 2.25
puts f.head      # 14
puts f.peak      # peak

# (The builtin side -- Thread#value through a poly container when NO user
# class defines the name -- is covered by thread_value_through_container.rb.
# A program mixing BOTH a user `value` reader and poly-carried Threads still
# hits the missing Fiber arm in the dispatch switch: a separate, pre-existing
# gap, out of scope here.)
