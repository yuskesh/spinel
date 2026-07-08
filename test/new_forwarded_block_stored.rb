# A forwarded `&proc` into a class whose `initialize` stores the block
# (`def initialize(&blk); @blk = blk; end`) threads the proc value into the
# constructor, and a later `@blk.call` returns the right value for every body
# return type -- faithful now that a proc's result rides the universal boxed
# return channel (this used to be a loud compile reject).
class Box
  def initialize(&blk)
    @blk = blk
  end

  def has_block?
    !@blk.nil?
  end

  def run(x)
    @blk.call(x)
  end
end

pr = ->(n) { n * 2 }
p Box.new(&pr).run(21)          # 42  (int return)

# String and array returns exercise the boxed channel for non-int results.
sr = ->(n) { "n=#{n}" }
p Box.new(&sr).run(7)           # "n=7"

ar = ->(n) { [n, n * 2] }
p Box.new(&ar).run(3)           # [3, 6]

# A lambda literal forwarded directly with &.
p Box.new(&->(n) { n + 100 }).run(5)   # 105

# An omitted block: the stored @blk is nil.
p Box.new.has_block?            # false
p Box.new(&pr).has_block?       # true

# A proc (not lambda) forwarded.
inc = proc { |n| n + 1 }
p Box.new(&inc).run(9)          # 10

# Forwarding into an initialize that also takes a positional arg.
class Labeled
  def initialize(label, &blk)
    @label = label
    @blk = blk
  end

  def call(x)
    "#{@label}=#{@blk.call(x)}"
  end
end

dbl = ->(n) { n * 2 }
puts Labeled.new("twice", &dbl).call(21)   # twice=42

# A proc pulled from a poly slot (a container element, a to-be-erased value)
# arrives boxed rather than as a bare sp_Proc*; forwarding it with & must unbox
# it into the stored block, not silently thread a NULL block.
procs = [->(n) { n * 2 }, ->(n) { n + 100 }]
p Box.new(&procs[0]).run(21)    # 42
p Box.new(&procs[1]).run(5)     # 105

table = { double: ->(n) { n * 2 }, negate: ->(n) { -n } }
p Box.new(&table[:double]).run(6)   # 12
p Box.new(&table[:negate]).run(6)   # -6

# A poly-carried non-lambda proc forwarded, and one alongside a positional arg.
plain = [proc { |n| n + 1 }]
p Box.new(&plain[0]).run(9)                    # 10
puts Labeled.new("via", &procs[0]).call(4)     # via=8
