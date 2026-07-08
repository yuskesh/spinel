# A custom initialize that calls its block (yield / block_given? / a called
# &blk) runs its body during construction. For a Struct/Data value class this
# body was previously skipped -- the members were set member-wise from the raw
# args, dropping the block. It now runs inlined at the .new site, so the block
# drives the member values via super. Args are routed through a method so the
# values are not constant-folded at the call site.

Scaled = Struct.new(:a, :b) do
  def initialize(a, b)
    super(yield(a), b * 10)
  end
end

def make_scaled(a, b)
  Scaled.new(a, b) { |n| n * 3 }
end

s = make_scaled(5, 2)
p [s.a, s.b]

# A keyword-parameter yielding initialize (here on a plain class) binds each
# keyword param by name inside the inlined body -- not positionally, which
# would drop the whole keyword hash into the first param.
class Box
  def initialize(x:, y: 10)
    @v = yield(x) + y
  end
  def v = @v
end

def make_box(a)
  Box.new(x: a) { |n| n * 2 }
end

p make_box(5).v
p make_box(9).v

# block_given? inside a Struct initialize still constructs without a block.
Maybe = Struct.new(:n) do
  def initialize(n)
    super(block_given? ? yield(n) : n)
  end
end

p Maybe.new(4) { |x| x + 100 }.n
p Maybe.new(7).n
