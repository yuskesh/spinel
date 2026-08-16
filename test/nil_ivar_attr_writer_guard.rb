# An ivar that `initialize` sets to nil and that everything else writes through
# a GENERATED writer must not be folded away as "always nil".
#
# `if @x` is decided at compile time when every write to @x in the tree assigns
# nil -- the whole guarded branch is then dropped from the emitted C. The scan
# only sees InstanceVariableWriteNodes, and `attr_accessor :x` / `attr_writer :x`
# / a Struct member / `instance_variable_set(:@x, v)` write the slot with no such
# node anywhere, so the very common "nil in initialize, filled in later through
# the accessor" shape folded to false and lost the branch. Silently: the program
# compiled, ran, exited 0 and printed a different answer.

# 1. attr_accessor: the linked-list shape the fold used to delete
class N
  attr_accessor :l, :val
  def initialize(v)
    @val = v
    @l = nil
  end
  def total
    s = @val
    s += @l.val if @l
    s
  end
end
a = N.new(1)
a.l = N.new(2)
puts a.total                      # 3

b = N.new(10)
puts b.total                      # 10 -- the guard still answers false

# 2. attr_writer only (no reader), read through the bare ivar
class W
  attr_writer :slot
  def initialize; @slot = nil; end
  def state
    return "empty" unless @slot
    "held:#{@slot}"
  end
end
w = W.new
puts w.state                      # empty
w.slot = "x"
puts w.state                      # held:x

# 3. the same guard reached through the attr_reader rather than the ivar
class R
  attr_accessor :conf
  def initialize; @conf = nil; end
end
r = R.new
puts(r.conf ? "on" : "off")       # off
r.conf = "c"
puts(r.conf ? "on" : "off")       # on

# 4. instance_variable_set is a write too
class K
  def initialize; @x = nil; end
  def set(v); instance_variable_set(:@x, v); end
  def show; @x ? "yes" : "no"; end
end
k = K.new
puts k.show                       # no
k.set(1)
puts k.show                       # yes

# 5. an ivar nothing ever writes non-nil still folds (the optimisation is kept)
class Fold
  attr_reader :mode
  def initialize; @mode = nil; @n = 7; end
  def run
    return "profiling" if @mode
    "plain:#{@n}"
  end
end
puts Fold.new.run                 # plain:7
